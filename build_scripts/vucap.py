#!/usr/bin/env python3
"""Parse and summarise a PCSX2 .vucap capture (VU1/VIF1 inputs + XGKICK outputs).

The capture comes from the modified PCSX2 in F:\\PCSX2-src (pcsx2/DebugTools/VuCapture.cpp),
started with:  python build_scripts/pcsx2_ee.py --vucap OUT --frames N --wait

The C++ replay test (ps2xTest/src/ps2_vu1_capture_replay_tests.cpp) reads the file
directly; this script is the loss check and a quick look at what was recorded.

Usage:
    python vucap.py capture.vucap                 # summary + loss check
    python vucap.py capture.vucap --runs 5        # also print the first 5 runs
    python vucap.py capture.vucap --vif           # VIF command walk (split records, lengths)
"""

import argparse
import collections
import struct
import sys
import zlib

REC_NAMES = {1: "SNAP", 2: "VIF", 3: "RUNSTART", 4: "RUNEND", 5: "KICKSTART",
             6: "KICKDATA", 7: "VSYNC", 8: "MEMSYNC", 9: "END", 10: "DMASTART"}
STATE_SIZE = 760
VU1_SIZE = 0x4000

# PCSX2 nVifT: source bytes per vector for UNPACK type (cmd & 0xF).
NVIFT = [4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2]

VIF_NAMES = {0x00: "NOP", 0x01: "STCYCL", 0x02: "OFFSET", 0x03: "BASE", 0x04: "ITOP",
             0x05: "STMOD", 0x06: "MSKPATH3", 0x07: "MARK", 0x10: "FLUSHE", 0x11: "FLUSH",
             0x13: "FLUSHA", 0x14: "MSCAL", 0x15: "MSCALF", 0x17: "MSCNT", 0x20: "STMASK",
             0x30: "STROW", 0x31: "STCOL", 0x4A: "MPG", 0x50: "DIRECT", 0x51: "DIRECTHL"}


def parse_state(p, off=0):
    vf = [struct.unpack_from("<4f", p, off + i * 16) for i in range(32)]
    off += 32 * 16
    vi = struct.unpack_from("<32I", p, off)
    off += 32 * 4
    acc = struct.unpack_from("<4f", p, off)
    off += 16
    q, pp, mac, status, clip = struct.unpack_from("<fIIII", p, off)
    off += 20 + 12 * 4
    branch, branchpc, dbranchpc, takedelay, ebit = struct.unpack_from("<5I", p, off)
    off += 20
    cycle, vpu_stat, fbrst = struct.unpack_from("<QII", p, off)
    return {"vf": vf, "vi": vi, "acc": acc, "q": q, "mac": mac, "status": status,
            "clip": clip, "ebit": ebit, "cycle": cycle, "vpu_stat": vpu_stat, "fbrst": fbrst}


def read_records(path):
    data = open(path, "rb").read()
    if data[:8] != b"PS2VUCAP":
        raise SystemExit(f"{path}: bad magic {data[:8]!r}")
    version, _ = struct.unpack_from("<II", data, 8)
    if version != 1:
        raise SystemExit(f"{path}: unsupported version {version}")
    off = 16
    recs = []
    torn = None
    while off < len(data):
        if off + 8 > len(data):
            torn = f"torn record header at 0x{off:x}"
            break
        rtype = data[off]
        length = struct.unpack_from("<I", data, off + 4)[0]
        if off + 8 + length > len(data):
            torn = f"torn {REC_NAMES.get(rtype, rtype)} payload at 0x{off:x} (need {length})"
            break
        recs.append((rtype, data[off + 8:off + 8 + length]))
        off += 8 + length
    return recs, torn, len(data)


def vif_command_words(words, i, cycle):
    """Full length in words of the VIF command starting at words[i] (PCSX2 rules)."""
    cmd = words[i]
    op = (cmd >> 24) & 0x7F
    num = (cmd >> 16) & 0xFF
    imm = cmd & 0xFFFF
    if op == 0x20:
        return 2
    if op in (0x30, 0x31):
        return 5
    if op == 0x4A:
        return 1 + ((num or 256) * 2)
    if op in (0x50, 0x51):
        return 1 + (imm or 65536) * 4
    if (op & 0x60) == 0x60:
        vnum = num or 256
        gsize = NVIFT[op & 0xF]
        cl = cycle & 0xFF
        wl = (cycle >> 8) & 0xFF or 256
        if wl <= cl:
            size = (vnum * gsize + 3) // 4
        else:
            n = cl * (vnum // wl) + min(vnum % wl, cl)
            size = (n * gsize + 3) >> 2
        return 1 + size
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--runs", type=int, default=0, help="print the first N runs")
    ap.add_argument("--vif", action="store_true", help="walk VIF commands and report split records")
    args = ap.parse_args()

    recs, torn, size = read_records(args.capture)
    counts = collections.Counter(r for r, _ in recs)
    print(f"{args.capture}: {size} bytes, {len(recs)} records")
    print("  " + "  ".join(f"{REC_NAMES.get(k, k)}={v}" for k, v in sorted(counts.items())))

    # A VIF command still in progress when a new DMA starts: PS2Recomp parses each DMA
    # buffer on its own (no carry-over), so such a command would be dropped there.
    dma = [struct.unpack_from("<8I", p) for r, p in recs if r == 10]
    if dma:
        spans = [d for d in dma if d[4] != 0]
        modes = collections.Counter("chain" if (d[0] >> 2) & 1 else "normal" for d in dma)
        print(f"  DMA starts {len(dma)} ({', '.join(f'{k}={v}' for k, v in sorted(modes.items()))}), "
              f"with a VIF command pending: {len(spans)}")
        for d in spans[:10]:
            print(f"    chcr=0x{d[0]:08x} madr=0x{d[1]:08x} tadr=0x{d[2]:08x} qwc={d[3]} "
                  f"cmd=0x{d[4]:02x} tag.size={d[5]} irqoffset=0x{d[6]:08x} stalled={d[7]}")

    ok = True
    if torn:
        print(f"  LOSS: {torn}")
        ok = False
    if not recs or recs[0][0] != 1:
        print("  LOSS: first record is not SNAP")
        ok = False
    end = [p for r, p in recs if r == 9]
    if not end:
        print("  LOSS: no END record (capture cut short — VM shut down mid-capture?)")
        ok = False
    else:
        e_runs, e_kicks, e_vif, e_mem, e_frames, e_inrun = struct.unpack_from("<6I", end[-1])
        got = (counts[3], counts[6], counts[2], counts[8], counts[7])
        want = (e_runs, e_kicks, e_vif, e_mem, e_frames)
        if got != want:
            print(f"  LOSS: END says runs/kicks/vif/memsync/frames={want}, file has {got}")
            ok = False
        if e_inrun:
            print("  note: capture ended inside a VU1 run (its RUNEND is missing)")

    snap = recs[0][1] if recs and recs[0][0] == 1 else None
    cycle = 0
    if snap:
        _, frame, flags = struct.unpack_from("<III", snap, 0)
        regsize = struct.unpack_from("<I", snap, 12 + STATE_SIZE)[0]
        regs = snap[16 + STATE_SIZE:16 + STATE_SIZE + regsize]
        cycle = struct.unpack_from("<I", regs, 4 * 16)[0]
        micro = snap[16 + STATE_SIZE + regsize:16 + STATE_SIZE + regsize + VU1_SIZE]
        mem = snap[16 + STATE_SIZE + regsize + VU1_SIZE:]
        print(f"  SNAP frame={frame} flags={flags:#x} (1=instant 2=recVU1 4=MTVU) vifregs={regsize}B "
              f"micro_crc={zlib.crc32(micro):08x} mem_crc={zlib.crc32(mem):08x}")
        if flags & 6:
            print("  LOSS: capture taken with VU1 recompiler or MTVU on")
            ok = False

    # Runs.
    starts = {}
    ends = {}
    kicks = collections.defaultdict(list)
    tpcs = collections.Counter()
    full = 0
    for r, p in recs:
        if r == 3:
            run, tpc, top, itop, cmem, cmic, has = struct.unpack_from("<7I", p)
            starts[run] = (tpc, top, itop, cmem, cmic, has)
            tpcs[tpc] += 1
            full += has
        elif r == 4:
            run, reason = struct.unpack_from("<II", p)
            ends[run] = (reason, parse_state(p, 8))
        elif r == 6:
            run, addr, sz, eop = struct.unpack_from("<4I", p)
            kicks[run].append((addr, sz, eop))
    missing_end = [k for k in starts if k not in ends]
    reasons = collections.Counter(v[0] for v in ends.values())
    frames = max(counts[7], 1)
    print(f"  runs={len(starts)} ({len(starts) / frames:.1f}/frame) fullmem={full} "
          f"end reasons={dict(reasons)} (0=E-bit 1=forced) missing RUNEND={len(missing_end)}")
    print("  start pcs: " + ", ".join(f"{k:#05x}x{v}" for k, v in tpcs.most_common(8)))
    if kicks:
        per = [len(v) for v in kicks.values()]
        print(f"  kick chunks: {sum(per)} over {len(kicks)} runs (max {max(per)}/run)")

    # VIF walk.
    vif_cmds = collections.Counter()
    split = 0
    pending = 0  # words still owed by a command that spilled over a record boundary
    mscal = 0
    for r, p in recs:
        if r != 2:
            continue
        n = struct.unpack_from("<I", p)[0]
        words = struct.unpack_from(f"<{n}I", p, 4)
        i = 0
        if pending:
            take = min(pending, n)
            pending -= take
            i = take
            split += 1
        while i < n:
            cmd = words[i]
            op = (cmd >> 24) & 0x7F
            vif_cmds[op] += 1
            if op in (0x14, 0x15, 0x17):
                mscal += 1
            if op == 0x01:
                cycle = cmd & 0xFFFF
            length = vif_command_words(words, i, cycle)
            if i + length > n:
                pending = i + length - n
                i = n
            else:
                i += length
    print(f"  VIF: MSCAL/MSCALF/MSCNT={mscal} vs runs={len(starts)}; records continuing a split command={split}"
          f"{'; ends mid-command' if pending else ''}")
    if args.vif:
        for op, c in sorted(vif_cmds.items(), key=lambda kv: -kv[1]):
            name = VIF_NAMES.get(op, f"UNPACK{op & 0x1F:02x}" if (op & 0x60) == 0x60 else f"?{op:02x}")
            print(f"    {name:10s} {c}")

    for run in sorted(starts)[:args.runs]:
        tpc, top, itop, cmem, cmic, has = starts[run]
        reason, st = ends.get(run, (None, None))
        print(f"  run {run}: tpc={tpc:#05x} top={top:#x} itop={itop:#x} mem_crc={cmem:08x} "
              f"micro_crc={cmic:08x} fullmem={has} end={reason} kicks={kicks.get(run, [])[:4]}")
        if st:
            print(f"    end vi[1..15]={[v & 0xFFFF for v in st['vi'][1:16]]} vf1={st['vf'][1]}")

    print("  loss check: " + ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
