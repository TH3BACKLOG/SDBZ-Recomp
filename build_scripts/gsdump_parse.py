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


# GSState::Freeze v9 begins with u32 version, then GSDrawingEnvironment as packed
# u64s (verified on a SLUS-21442 dump: PRIM@4, COLCLAMP@0x44, ctx0@0x7c with
# XYOFFSET=0x7000/0x7200, SCISSOR@+0x30, ALPHA@+0x38=0x44, FRAME@+0x50), ctx
# stride 0x60.  The packet stream only re-sends what the game changed, so
# anything still at its freeze value (often ALPHA_1) is missing from a replay
# unless it is injected first.
# Freeze order (GSState.cpp Freeze): PRIM PRMODECONT TEXCLUT SCANMSK TEXA FOGCOL DIMX DTHE
# COLCLAMP PABE BITBLTBUF TRXDIR TRXPOS TRXREG TRXREG. TEXCLUT is 0x14, not 0x1C (0x1C is
# SCANMSK; fixed 10-02, it used to load SCANMSK's value into TEXCLUT).
_ENV_REGS = ((0x04, 0x00), (0x0C, 0x1A), (0x14, 0x1C), (0x1C, 0x22), (0x24, 0x3B), (0x2C, 0x3D),
             (0x34, 0x44), (0x3C, 0x45), (0x44, 0x46), (0x4C, 0x49))  # PRIM PRMODECONT TEXCLUT SCANMSK TEXA FOGCOL DIMX DTHE COLCLAMP PABE
# Transfer regs are restored in the freeze only (writing TRXDIR in a packet would start a transfer).
_XFER_REGS = ((0x54, 0x50), (0x64, 0x51), (0x6C, 0x52), (0x74, 0x52))  # BITBLTBUF TRXPOS TRXREG TRXREG(obsolete copy)
_V_REGS = ((0x13C, 0x01), (0x144, 0x02), (0x14C, 0x03), (0x154, 0x0A))  # m_v RGBAQ ST UV FOG
_CTX_REGS = ((0x00, 0x18), (0x08, 0x06), (0x10, 0x14), (0x18, 0x08), (0x20, 0x34),
             (0x28, 0x36), (0x30, 0x40), (0x38, 0x42), (0x40, 0x47), (0x48, 0x4A),
             (0x50, 0x4C), (0x58, 0x4E))  # XYOFFSET TEX0 TEX1 CLAMP MIPTBP1/2 SCISSOR ALPHA TEST FBA FRAME ZBUF
_CTX_BASE = 0x7C
_CTX_STRIDE = 0x60


def init_state_packet(state):
    """One GIF PACKED A+D transfer that restores the freeze's register state."""
    if struct.unpack_from("<I", state, 0)[0] != 9:
        raise SystemExit("--init-state: only freeze version 9 handled")
    ad = []
    for off, reg in _ENV_REGS:
        ad.append((struct.unpack_from("<Q", state, off)[0], reg))
    for c in (0, 1):
        for off, reg in _CTX_REGS:
            ad.append((struct.unpack_from("<Q", state, _CTX_BASE + c * _CTX_STRIDE + off)[0], reg + c))
    tag = len(ad) | (1 << 15) | (0 << 58) | (1 << 60) | (0xE << 64)  # NLOOP, EOP, FLG=PACKED, NREG=1, REGS=A+D
    blob = struct.pack("<Q", tag & 0xFFFFFFFFFFFFFFFF) + struct.pack("<Q", 0xE)
    for data, reg in ad:
        blob += struct.pack("<QQ", data, reg)
    return {"id": PACKET_TRANSFER, "path": 0, "size": len(blob), "data": blob, "offset": 0}


def emit_replay(info, regs, packets, out_path, init_state=None):
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
    if init_state is not None:
        transfers.insert(0, init_state_packet(init_state))
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
    if init_state is not None:
        transfers.insert(0, init_state_packet(init_state))
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


VRAM_SIZE = 4 * 1024 * 1024
# GSState::Freeze (state version 9) ends with VRAM, then 4 x (GIF tag 16 +
# reg u32 4) path records, then Q (float). Anchor on the end so the size of the
# register header in front of VRAM does not matter.
STATE_TAIL_V9 = 4 * (16 + 4) + 4


def emit_vram(state, out_path):
    version = struct.unpack_from("<I", state, 0)[0]
    if version != 9:
        raise SystemExit(f"--emit-vram: state version {version} not handled (only 9)")
    start = len(state) - STATE_TAIL_V9 - VRAM_SIZE
    if start < 4:
        raise SystemExit(f"--emit-vram: state too small ({len(state)} bytes)")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(state[start:start + VRAM_SIZE])
    print(f"wrote {VRAM_SIZE} bytes of VRAM (state offset {start}) to {out_path}")


# ---- .gsr -> PCSX2 .gs (so PCSX2's GSRunner can render our own captures as an oracle) ----

# Default freeze template: any real SDBZ PCSX2 dump. Only its size and version are used;
# every register, m_tr, path tag and VRAM byte is overwritten.
TEMPLATE_DUMP = Path(r"F:\SDBZ Recomp\PCSX2\snaps\Super Dragon Ball Z_SLUS-21442_20260423093815.gs.zst")
GS_PATH3 = 2  # GSTransferPath::Path3 (Path1Old=0 is capped at 16 KB by the replayer)


def read_gsr(path):
    """-> (regs, [(path, payload bytes)]) from a GSR1 container."""
    raw = Path(path).read_bytes()
    if raw[:4] != GSR_MAGIC:
        raise SystemExit(f"{path} is not a GSR1 file")
    _ver, count, regs_size, payload_size, _ = struct.unpack_from("<5I", raw, 4)
    p = 24
    regs = raw[p:p + regs_size]
    p += regs_size
    idx = struct.unpack_from(f"<{count * 3}I", raw, p)
    p += count * 12
    payload = raw[p:p + payload_size]
    return regs, [(idx[3 * i + 2], payload[idx[3 * i]:idx[3 * i] + idx[3 * i + 1]]) for i in range(count)]


def ad_pairs(blob):
    """Register values from one PACKED A+D GIF tag (the synthetic transfer 0 of a .gsr)."""
    lo, hi = struct.unpack_from("<QQ", blob, 0)
    nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, (lo >> 60) & 0xF
    if flg != 0 or nreg != 1 or (hi & 0xF) != 0xE or 16 + nloop * 16 > len(blob):
        raise SystemExit("transfer 0 is not a single PACKED A+D tag: not a register-restore packet")
    vals = {}
    for i in range(nloop):
        data, addr = struct.unpack_from("<QQ", blob, 16 + i * 16)
        vals[addr & 0xFF] = data
    return vals


def build_freeze(template_state, vram, regvals):
    """GSState::Freeze v9 image: template size, registers from regvals, our VRAM, idle paths."""
    if struct.unpack_from("<I", template_state, 0)[0] != 9:
        raise SystemExit("template freeze is not version 9")
    vstart = len(template_state) - STATE_TAIL_V9 - VRAM_SIZE
    if len(vram) != VRAM_SIZE:
        raise SystemExit(f"VRAM image is {len(vram)} bytes, expected {VRAM_SIZE}")
    st = bytearray(len(template_state))
    struct.pack_into("<I", st, 0, 9)
    slots = list(_ENV_REGS) + list(_XFER_REGS) + list(_V_REGS)
    for c in (0, 1):
        slots += [(_CTX_BASE + c * _CTX_STRIDE + off, reg + c) for off, reg in _CTX_REGS]
    for off, reg in slots:
        if reg in regvals:
            struct.pack_into("<Q", st, off, regvals[reg])
    if 0x01 not in regvals:
        struct.pack_into("<Q", st, 0x13C, 0x3F800000_80808080)  # RGBAQ, Q=1.0
    # m_tr stays zero (total=0: no transfer in flight); GIF path tags zero (NLOOP=0: idle).
    st[vstart:vstart + VRAM_SIZE] = vram
    struct.pack_into("<f", st, len(st) - 4, 1.0)  # m_q
    return bytes(st)


def probe_packet(fbp, fbw, psm, w, h):
    """Set up a GS->host read of buffer fbp (w x h at 0,0). Followed by a ReadFIFO2 packet,
    GSRunner `-dump tr` saves it as `*_read_<SBP>_*.bmp` straight from VRAM, so no draw
    state can hide it. (A 1-pixel sprite probe was silently dropped by PCSX2 for FBP 0.)"""
    ad = [
        ((fbp * 32) | fbw << 16 | psm << 24, 0x50),           # BITBLTBUF: SBP (blocks), SBW, SPSM
        (0, 0x51),                                            # TRXPOS: source (0,0)
        (w | h << 32, 0x52),                                  # TRXREG
        (1, 0x53),                                            # TRXDIR: local -> host
    ]
    tag = len(ad) | (1 << 15) | (1 << 60)
    blob = struct.pack("<QQ", tag, 0xE)
    for data, reg in ad:
        blob += struct.pack("<QQ", data, reg)
    return blob


def emit_gs(gsr_path, out_path, template=TEMPLATE_DUMP, stop=None, probe=None):
    """Write a PCSX2 .gs (new header, uncompressed) from a .gsr + sibling .vram.

    stop:  only transfers [0, stop) (same meaning as PS2X_GSBENCH_STOP)
    probe: (fbp, fbw, psm, w, h) -> a VSync, probe_packet + ReadFIFO2, then a final VSync.
           Render with GSRunner `-renderer sw -dump tr -loop 1`: `*_read_*.bmp` is buffer
           fbp at the cut."""
    regs, transfers = read_gsr(gsr_path)
    vram_path = Path(gsr_path).with_suffix(".vram")
    if not vram_path.exists():
        raise SystemExit(f"missing {vram_path}: the oracle needs the starting VRAM")
    traw = decompress(template, template.read_bytes())
    tinfo, tstate, _tregs, _ = parse_container(traw)
    freeze = build_freeze(tstate, vram_path.read_bytes(), ad_pairs(transfers[0][1]))
    if stop is not None:
        transfers = transfers[:stop]
    # Live captures record path 0 for everything: send them as PATH3. PCSX2-origin .gsr keep
    # their path, except an oversized Path1Old (the replayer drops those).
    all_zero = all(p == 0 for p, _ in transfers)
    serial = b"SLUS-21442"
    crc = tinfo.get("crc") or 0
    header = struct.pack("<9I", 9, len(freeze), 36, len(serial), crc, 0, 0, 36 + len(serial), 0)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<II", 0xFFFFFFFF, len(header) + len(serial)))
        f.write(header + serial)
        f.write(freeze)
        f.write(regs)

        def transfer(path, data):
            f.write(struct.pack("<BBI", PACKET_TRANSFER, path, len(data)))
            f.write(data)

        def vsync():
            f.write(struct.pack("<B", PACKET_REGISTERS))
            f.write(regs)
            f.write(struct.pack("<BB", PACKET_VSYNC, 0))

        for path, data in transfers:
            if all_zero or (path == 0 and len(data) > 16384):
                path = GS_PATH3
            transfer(path, data)
        if probe is not None:
            vsync()  # flush pending draws first
            transfer(GS_PATH3, probe_packet(*probe))
            _, _, _, pw, ph = probe
            bpp = {0: 32, 1: 24, 2: 16, 0xA: 16}.get(probe[2] & 0xF, 32)
            f.write(struct.pack("<BI", PACKET_READFB, (pw * ph * bpp // 8 + 15) // 16))
        vsync()
    print(f"wrote {out_path}: {len(transfers)} transfers{' + probe' if probe else ''}, freeze {len(freeze)} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--summary", action="store_true", help="print container + packet census")
    ap.add_argument("--emit-replay", type=Path, help="write a .gsr replay container for ps2x_tests")
    ap.add_argument("--emit-bin", type=Path, help="concatenate GIF payloads to a flat .bin")
    ap.add_argument("--emit-cpp", type=Path, help="emit a C++ .inc for a replay test")
    ap.add_argument("--dump-regs", type=Path, help="write the 8 KiB priv-register block")
    ap.add_argument("--init-state", action="store_true", help="with --emit-replay: prepend a synthetic packet restoring the freeze's GS registers (ALPHA/TEST/CLAMP..); shifts transfer indices by 1")
    ap.add_argument("--emit-vram", type=Path, help="write the 4 MiB VRAM image from the GS state (feeds ps2x_gs_bench)")
    ap.add_argument("--path", type=int, choices=[0, 1, 2], help="restrict --emit-bin to one GIF path")
    ap.add_argument("--max-packets", type=int, help="stop after N packets (for probing a suspect dump)")
    ap.add_argument("--gsr-to-gs", type=Path, metavar="OUT.gs", help="input is a .gsr (+ sibling .vram): write a PCSX2 .gs that GSRunner can replay")
    ap.add_argument("--template", type=Path, default=TEMPLATE_DUMP, help="with --gsr-to-gs: real PCSX2 dump whose freeze size/version is reused")
    ap.add_argument("--stop", type=int, help="with --gsr-to-gs: keep transfers [0, N) only")
    ap.add_argument("--probe", metavar="FBP,FBW,PSM,W,H", help="with --gsr-to-gs: append the 1-pixel probe draw into buffer FBP (pages)")
    args = ap.parse_args()

    if args.dump.suffix.lower() == ".gsr":
        if not args.gsr_to_gs:
            raise SystemExit("a .gsr input needs --gsr-to-gs OUT.gs")
        probe = tuple(int(v, 0) for v in args.probe.split(",")) if args.probe else None
        emit_gs(args.dump, args.gsr_to_gs, args.template, args.stop, probe)
        return

    raw = decompress(args.dump, args.dump.read_bytes())
    info, state, regs, reader = parse_container(raw)
    try:
        packets = parse_packets(reader, limit=args.max_packets)
    except (EOFError, ValueError) as exc:
        print(f"packet stream stopped: {exc}", file=sys.stderr)
        reader.pos = 0
        raise SystemExit(2)

    if args.summary or not (args.emit_replay or args.emit_bin or args.emit_cpp or args.dump_regs or args.emit_vram):
        summarize(info, state, regs, packets)
    if args.emit_replay:
        emit_replay(info, regs, packets, args.emit_replay, state if args.init_state else None)
    if args.emit_bin:
        emit_bin(packets, args.emit_bin, args.path)
    if args.emit_cpp:
        emit_cpp(info, regs, packets, args.emit_cpp)
    if args.dump_regs:
        args.dump_regs.write_bytes(regs)
        print(f"wrote {len(regs)} bytes to {args.dump_regs}")
    if args.emit_vram:
        emit_vram(state, args.emit_vram)


if __name__ == "__main__":
    main()
