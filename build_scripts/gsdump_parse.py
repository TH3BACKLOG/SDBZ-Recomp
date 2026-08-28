#!/usr/bin/env python3
"""
gsdump_parse.py -- read a PCSX2 GS dump (.gs / .gs.xz / .gs.zst) and turn it
into something our GS can be driven by.

Why this exists
---------------
Stage 5.11 diagnostics currently cost ~48 min of build plus a 90 s game run per
hypothesis. A GS dump is a recording of one frame's worth of GIF traffic plus
the GS register state that produced it. Replaying that same byte stream through
our own GS gives byte-exact ground truth for the memory-card screen without the
game, the runner, or the wait.

  PCSX2: pause on the memory-card screen -> Debug -> "Save Single Frame GS Dump"

Usage
-----
  python gsdump_parse.py dump.gs --summary
  python gsdump_parse.py dump.gs --emit-replay frame.gsr   <-- feeds ps2x_tests
  python gsdump_parse.py dump.gs --emit-bin gifpath.bin
  python gsdump_parse.py dump.gs --emit-cpp gsdump_replay_data.inc
  python gsdump_parse.py dump.gs --dump-regs regs.bin

`--emit-replay` is the one to use. It writes a self-describing binary that
`ps2xTest/src/ps2_gsdump_replay_tests.cpp` loads at runtime, so re-capturing a
dump costs zero recompilation. `--emit-cpp` bakes the same data into a C++
header instead, which is only worth it for a permanently-checked-in fixture --
a full frame is megabytes of source.

VALIDATION STATUS
-----------------
The container layout below follows PCSX2's GSDumpFile. PCSX2 has shipped two
on-disk generations and the older one has no magic to key off, so the reader
sniffs and reports which branch it took. **Check the --summary output against
the dump you captured before trusting any emitted data.** A dump that parses
into a plausible packet count with sane path/size fields is good; one that
produces a single enormous "transfer" or an unknown packet id immediately is
the reader guessing wrong, not the dump being broken.
"""

import argparse
import struct
import sys
from pathlib import Path

GS_REG_BLOCK_SIZE = 8192  # PCSX2 dumps the full privileged register block

PACKET_TRANSFER = 0
PACKET_VSYNC = 1
PACKET_READFB = 2
PACKET_REGISTERS = 3

PACKET_NAMES = {
    PACKET_TRANSFER: "Transfer",
    PACKET_VSYNC: "VSync",
    PACKET_READFB: "ReadFB",
    PACKET_REGISTERS: "Registers",
}

# GIF transfer paths. PATH3 is the one that carries most texture/CLUT uploads.
PATH_NAMES = {0: "PATH1", 1: "PATH2", 2: "PATH3", 3: "PATH1(new)"}

# Container magic for --emit-replay. Bumped if the layout below ever changes;
# the C++ loader rejects anything it does not recognise rather than guessing.
GSR_MAGIC = b"GSR1"
GSR_VERSION = 1


class Reader:
    """Bounds-checked little-endian cursor over the dump body."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def remaining(self):
        return len(self.data) - self.pos

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise EOFError(
                f"wanted {n} bytes at offset {self.pos}, only {self.remaining()} left"
            )
        chunk = self.data[self.pos : self.pos + n]
        self.pos += n
        return chunk

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack_from("<I", self.take(4))[0]


def decompress(path, raw):
    """Transparently handle .xz and .zst dumps."""
    if path.suffix == ".xz" or raw[:6] == b"\xfd7zXZ\x00":
        import lzma

        return lzma.decompress(raw)
    if path.suffix == ".zst" or raw[:4] == b"\x28\xb5\x2f\xfd":
        try:
            import zstandard
        except ImportError:
            sys.exit(
                "this dump is zstd-compressed; install the 'zstandard' package "
                "or re-export the dump uncompressed from PCSX2"
            )
        return zstandard.ZstdDecompressor().stream_reader(raw).read()
    return raw


def parse_container(raw):
    """
    Split the dump into (info, state_bytes, regs_bytes, packet_reader).

    Old format:  state_size:u32, state[state_size], regs[8192], packets

    New format:  0xFFFFFFFF, header_size:u32, header[header_size],
                 state[state_size], regs[8192], packets

    where the first 36 bytes of the header block are nine u32s:
        state_version, state_size, serial_offset, serial_size, crc,
        screenshot_width, screenshot_height, screenshot_offset,
        screenshot_size
    and serial_offset / screenshot_offset are relative to the START of the
    header block. header_size covers the struct plus the serial string plus
    the screenshot, so 36 + serial_size + screenshot_size == header_size is a
    free self-check -- it is asserted below, because getting this layout wrong
    silently yields a plausible-looking but garbage packet stream.
    """
    r = Reader(raw)
    info = {}

    first = r.u32()
    if first == 0xFFFFFFFF:
        info["format"] = "new (0xFFFFFFFF marker)"
        header_size = r.u32()
        info["header_size"] = header_size
        header_start = r.pos
        header = r.take(header_size)
        fields = struct.unpack_from("<9I", header, 0)
        (
            state_version,
            state_size,
            serial_offset,
            serial_size,
            crc,
            shot_w,
            shot_h,
            shot_offset,
            shot_size,
        ) = fields
        info["state_version"] = state_version
        info["crc"] = crc
        info["serial"] = header[serial_offset : serial_offset + serial_size].decode(
            "ascii", "replace"
        )
        info["screenshot"] = f"{shot_w}x{shot_h} ({shot_size} bytes)"
        info["header_start"] = header_start

        expected = 36 + serial_size + shot_size
        if expected != header_size:
            raise ValueError(
                f"header self-check failed: 36 + serial({serial_size}) + "
                f"screenshot({shot_size}) = {expected}, but header_size is "
                f"{header_size} -- the field order above is wrong for this dump"
            )
    else:
        info["format"] = "old (no marker)"
        info["state_version"] = None
        info["crc"] = None
        state_size = first

    if state_size > r.remaining():
        raise ValueError(
            f"state_size {state_size} exceeds the {r.remaining()} bytes left -- "
            "the container branch above is almost certainly wrong for this dump"
        )
    info["state_size"] = state_size
    state = r.take(state_size)
    regs = r.take(GS_REG_BLOCK_SIZE)
    info["packets_start"] = r.pos
    return info, state, regs, r


def parse_packets(r, limit=None):
    packets = []
    while r.remaining() > 0:
        if limit is not None and len(packets) >= limit:
            break
        offset = r.pos
        pid = r.u8()
        if pid == PACKET_TRANSFER:
            path = r.u8()
            size = r.u32()
            data = r.take(size)
            packets.append(
                {"id": pid, "offset": offset, "path": path, "size": size, "data": data}
            )
        elif pid == PACKET_VSYNC:
            packets.append({"id": pid, "offset": offset, "field": r.u8()})
        elif pid == PACKET_READFB:
            # GSReadFBData: two 32-bit coordinate pairs.
            packets.append({"id": pid, "offset": offset, "data": r.take(16)})
        elif pid == PACKET_REGISTERS:
            packets.append(
                {"id": pid, "offset": offset, "data": r.take(GS_REG_BLOCK_SIZE)}
            )
        else:
            raise ValueError(
                f"unknown packet id {pid} at offset {offset} after "
                f"{len(packets)} packets -- reader is out of sync with the dump"
            )
    return packets


def summarize(info, state, regs, packets):
    print("== container ==")
    for k, v in info.items():
        print(f"  {k:<12} {v}")
    print(f"  {'state':<12} {len(state)} bytes")
    print(f"  {'regs':<12} {len(regs)} bytes")

    print("\n== packets ==")
    counts = {}
    transfer_bytes = {}
    for p in packets:
        name = PACKET_NAMES.get(p["id"], f"id{p['id']}")
        counts[name] = counts.get(name, 0) + 1
        if p["id"] == PACKET_TRANSFER:
            pname = PATH_NAMES.get(p["path"], f"path{p['path']}")
            transfer_bytes[pname] = transfer_bytes.get(pname, 0) + p["size"]
    print(f"  total {len(packets)}")
    for name, n in sorted(counts.items()):
        print(f"    {name:<12} {n}")
    if transfer_bytes:
        print("\n== GIF transfer bytes by path ==")
        for pname, n in sorted(transfer_bytes.items()):
            print(f"    {pname:<8} {n}")

    print("\n== first 12 packets ==")
    for p in packets[:12]:
        name = PACKET_NAMES.get(p["id"], f"id{p['id']}")
        extra = ""
        if p["id"] == PACKET_TRANSFER:
            extra = f" {PATH_NAMES.get(p['path'], p['path'])} size={p['size']}"
        elif p["id"] == PACKET_VSYNC:
            extra = f" field={p['field']}"
        print(f"    @{p['offset']:<10} {name}{extra}")

    # A sane frame dump has a few hundred to a few thousand packets and at
    # least one VSync. Anything else means the reader guessed the container
    # wrong -- see VALIDATION STATUS at the top of this file.
    if len(packets) < 4 or counts.get("VSync", 0) == 0:
        print(
            "\n  !! this does not look like a valid frame dump "
            "(too few packets, or no VSync). Do not trust emitted data.",
            file=sys.stderr,
        )


def emit_bin(packets, out_path, path_filter=None):
    """Concatenate GIF transfer payloads into one flat binary."""
    with open(out_path, "wb") as f:
        n = 0
        for p in packets:
            if p["id"] != PACKET_TRANSFER:
                continue
            if path_filter is not None and p["path"] != path_filter:
                continue
            f.write(p["data"])
            n += 1
    print(f"wrote {n} transfer payloads to {out_path}")


def emit_replay(info, regs, packets, out_path):
    """
    Write a .gsr replay container that ps2x_tests loads at runtime.

    Layout (all little-endian, no padding beyond what is written):

        magic[4]        "GSR1"
        version:u32     1
        count:u32       number of GIF transfers
        regsSize:u32    size of the privileged register block (8192)
        payloadSize:u32 total GIF payload bytes
        reserved:u32    0
        regs[regsSize]
        index[count]    { offset:u32, size:u32, path:u32 }
        payload[payloadSize]

    `offset` is relative to the start of the payload block, so the loader can
    mmap/read the payload once and hand slices straight to
    GS::processGIFPacket(const uint8_t *, uint32_t).
    """
    transfers = [p for p in packets if p["id"] == PACKET_TRANSFER]
    blob = bytearray()
    index = bytearray()
    for p in transfers:
        index.extend(struct.pack("<III", len(blob), p["size"], p["path"]))
        blob.extend(p["data"])

    with open(out_path, "wb") as f:
        f.write(GSR_MAGIC)
        f.write(struct.pack("<IIIII", GSR_VERSION, len(transfers), len(regs), len(blob), 0))
        f.write(regs)
        f.write(index)
        f.write(blob)
    print(f"wrote {len(transfers)} transfers / {len(blob)} payload bytes to {out_path}")


def emit_cpp(info, regs, packets, out_path):
    """
    Emit a C++ include that a replay test can feed to a bare GS.

    Consume it from a test like:

        #include "gsdump_replay_data.inc"
        GS gs;
        gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
        for (const auto &t : kGsDumpTransfers)
            gs.processGIFPacket(kGsDumpPayload + t.offset, t.size);

    Note processGIFPacket takes no path argument -- the GIF path is recorded in
    the table for triage only.
    """
    transfers = [p for p in packets if p["id"] == PACKET_TRANSFER]
    blob = bytearray()
    entries = []
    for p in transfers:
        entries.append((len(blob), p["size"], p["path"]))
        blob.extend(p["data"])

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("// Generated by build_scripts/gsdump_parse.py -- do not edit.\n")
        f.write(f"// source format: {info.get('format')}, crc: {info.get('crc'):#x}\n")
        f.write("#pragma once\n#include <cstdint>\n#include <cstddef>\n\n")
        f.write("struct GsDumpTransfer { size_t offset; uint32_t size; uint8_t path; };\n\n")
        f.write("inline const uint8_t kGsDumpPayload[] = {\n")
        for i in range(0, len(blob), 16):
            row = ", ".join(f"0x{b:02X}" for b in blob[i : i + 16])
            f.write(f"    {row},\n")
        f.write("};\n\n")
        f.write("inline const GsDumpTransfer kGsDumpTransfers[] = {\n")
        for off, size, path in entries:
            f.write(f"    {{{off}, {size}, {path}}},\n")
        f.write("};\n\n")
        f.write("inline const uint8_t kGsDumpPrivRegs[] = {\n")
        for i in range(0, len(regs), 16):
            row = ", ".join(f"0x{b:02X}" for b in regs[i : i + 16])
            f.write(f"    {row},\n")
        f.write("};\n")
    print(
        f"wrote {len(entries)} transfers / {len(blob)} payload bytes to {out_path}"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--summary", action="store_true", help="print container + packet census")
    ap.add_argument("--emit-replay", type=Path, help="write a .gsr replay container for ps2x_tests")
    ap.add_argument("--emit-bin", type=Path, help="concatenate GIF payloads to a flat .bin")
    ap.add_argument("--emit-cpp", type=Path, help="emit a C++ .inc for a replay test")
    ap.add_argument("--dump-regs", type=Path, help="write the 8 KiB priv-register block")
    ap.add_argument("--path", type=int, choices=[0, 1, 2], help="restrict --emit-bin to one GIF path")
    ap.add_argument("--max-packets", type=int, help="stop after N packets (for probing a suspect dump)")
    args = ap.parse_args()

    raw = decompress(args.dump, args.dump.read_bytes())
    info, state, regs, reader = parse_container(raw)
    try:
        packets = parse_packets(reader, limit=args.max_packets)
    except (EOFError, ValueError) as exc:
        print(f"packet stream stopped: {exc}", file=sys.stderr)
        reader.pos = 0
        raise SystemExit(2)

    if args.summary or not (args.emit_replay or args.emit_bin or args.emit_cpp or args.dump_regs):
        summarize(info, state, regs, packets)
    if args.emit_replay:
        emit_replay(info, regs, packets, args.emit_replay)
    if args.emit_bin:
        emit_bin(packets, args.emit_bin, args.path)
    if args.emit_cpp:
        emit_cpp(info, regs, packets, args.emit_cpp)
    if args.dump_regs:
        args.dump_regs.write_bytes(regs)
        print(f"wrote {len(regs)} bytes to {args.dump_regs}")


if __name__ == "__main__":
    main()
