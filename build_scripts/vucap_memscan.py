#!/usr/bin/env python3
"""Scan VU1 data memory at every run start of a .vucap capture for bad floats.

Reads RUNSTART records that carry the full 0x4000-byte VU1 data memory
(hasMem=1): the runtime recorder (PS2X_VUCAP, PS2X_VUCAP_FULLMEM=1, the default)
or a PCSX2 capture taken with full memory.

Per start pc (tpc) it counts, per qword lane, values that are NaN/Inf
(exponent 0xFF), huge (|x| >= --huge) or denormal, and keeps the largest |x|.
With two captures it lists, for start pcs both have, the qwords that are bad
in one capture and never bad in the other, and qwords whose largest |x| differs
by more than --ratio. The two captures need not be the same frames: matrices
live at fixed qwords per microprogram, vertex data does not.

  python vucap_memscan.py ours.vucap
  python vucap_memscan.py ours.vucap pcsx2.vucap
  python vucap_memscan.py pcsx2.vucap --control 0x084:17   # negative control

--control TPC:QWORD writes a NaN into lane x of that qword in every run with
that tpc before scanning; the report must then show it.
"""

import argparse
import mmap
import struct
import sys

import numpy as np

STATE_SIZE = 760
VU1_SIZE = 0x4000
QWORDS = VU1_SIZE // 16
LANES = "xyzw"


class TpcStats:
    def __init__(self):
        self.runs = 0
        self.nonfinite = np.zeros((QWORDS, 4), np.int64)
        self.huge = np.zeros((QWORDS, 4), np.int64)
        self.denormal = np.zeros((QWORDS, 4), np.int64)
        self.maxabs = np.zeros((QWORDS, 4), np.float64)
        self.first_bad = {}  # qword -> (run, raw lanes)


def scan(path, huge, control):
    stats = {}
    runs_total = runs_nomem = 0
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        if m[:8] != b"PS2VUCAP":
            raise SystemExit(f"{path}: bad magic")
        off, n = 16, len(m)
        while off + 8 <= n:
            rtype = m[off]
            length = struct.unpack_from("<I", m, off + 4)[0]
            if off + 8 + length > n:
                print(f"  {path}: torn record at 0x{off:x}, stopping")
                break
            if rtype == 3:
                runs_total += 1
                run, tpc, _top, _itop, _cm, _cmi, has = struct.unpack_from("<7I", m, off + 8)
                if not has or length < 28 + STATE_SIZE + VU1_SIZE:
                    runs_nomem += 1
                else:
                    raw = np.frombuffer(m, dtype="<u4", count=VU1_SIZE // 4,
                                        offset=off + 8 + 28 + STATE_SIZE).reshape(QWORDS, 4).copy()
                    if control and control[0] == tpc:
                        raw[control[1], 0] = 0x7FC00000
                    s = stats.setdefault(tpc, TpcStats())
                    s.runs += 1
                    exp = (raw >> 23) & 0xFF
                    nonfin = exp == 0xFF
                    den = (exp == 0) & ((raw & 0x7FFFFFFF) != 0)
                    with np.errstate(invalid="ignore", over="ignore"):
                        vals = np.abs(raw.view("<f4").astype(np.float64))
                    vals[nonfin] = 0.0
                    big = vals >= huge
                    s.nonfinite += nonfin
                    s.huge += big
                    s.denormal += den
                    np.maximum(s.maxabs, vals, out=s.maxabs)
                    for q in np.nonzero((nonfin | big).any(axis=1))[0]:
                        if int(q) not in s.first_bad:
                            s.first_bad[int(q)] = (run, [f"{v:08x}" for v in raw[q]])
            off += 8 + length
    return stats, runs_total, runs_nomem


def bad_fraction(s):
    return ((s.nonfinite + s.huge).sum(axis=1) > 0).astype(np.float64), (s.nonfinite + s.huge).max(axis=1) / max(s.runs, 1)


def report(path, stats, runs_total, runs_nomem, top):
    print(f"{path}: runs={runs_total} without full memory={runs_nomem}")
    print("  note: VU1 memory also holds GIF tags and integers, which read as NaN/huge floats here;"
          " a qword bad in BOTH captures is normal -- compare two captures")
    if runs_total and runs_nomem == runs_total:
        print("  nothing to scan: no RUNSTART carries VU1 data memory")
    for tpc, s in sorted(stats.items(), key=lambda kv: -kv[1].runs):
        nf, hg, dn = int(s.nonfinite.sum()), int(s.huge.sum()), int(s.denormal.sum())
        _, frac = bad_fraction(s)
        badq = np.nonzero(frac > 0)[0]
        print(f"  tpc {tpc:#05x}: runs={s.runs} nonfinite lanes={nf} huge lanes={hg} denormal lanes={dn} "
              f"qwords ever bad={len(badq)}")
        for q in sorted(badq, key=lambda q: -frac[q])[:top]:
            lanes = "".join(LANES[i] for i in range(4) if s.nonfinite[q, i] or s.huge[q, i])
            run, rawv = s.first_bad.get(int(q), (None, None))
            print(f"    qword {q:4d} (0x{q * 16:04x}) bad in {frac[q] * 100:5.1f}% of runs, lanes {lanes}, "
                  f"max finite |x|={s.maxabs[q].max():.4g}, first run {run} raw {rawv}")


def compare(name_a, a, name_b, b, top, ratio):
    common = sorted(set(a) & set(b), key=lambda t: -(a[t].runs + b[t].runs))
    print(f"compare {name_a} (A) vs {name_b} (B): start pcs in both={len(common)}, "
          f"only A={sorted(hex(t) for t in set(a) - set(b))[:10]}, only B={sorted(hex(t) for t in set(b) - set(a))[:10]}")
    for tpc in common:
        sa, sb = a[tpc], b[tpc]
        _, fa = bad_fraction(sa)
        _, fb = bad_fraction(sb)
        only_a = [q for q in np.nonzero((fa > 0) & (fb == 0))[0]]
        only_b = [q for q in np.nonzero((fb > 0) & (fa == 0))[0]]
        ma, mb = sa.maxabs.max(axis=1), sb.maxabs.max(axis=1)
        with np.errstate(divide="ignore", invalid="ignore"):
            r = np.where((mb > 0) & (ma > 1.0), ma / mb, 0.0)
        grown = [q for q in np.argsort(-r) if r[q] > ratio][:top]
        print(f"  tpc {tpc:#05x}: runs A={sa.runs} B={sb.runs}; bad only in A={len(only_a)}, only in B={len(only_b)}, "
              f"|x| grew >{ratio:g}x in A={len(grown)}")
        for q in sorted(only_a, key=lambda q: -fa[q])[:top]:
            print(f"    A-only bad qword {q:4d}: {fa[q] * 100:.1f}% of A runs; max |x| A={ma[q]:.4g} B={mb[q]:.4g}")
        for q in sorted(only_b, key=lambda q: -fb[q])[:top]:
            print(f"    B-only bad qword {q:4d}: {fb[q] * 100:.1f}% of B runs; max |x| A={ma[q]:.4g} B={mb[q]:.4g}")
        for q in grown:
            print(f"    qword {q:4d}: max |x| A={ma[q]:.4g} B={mb[q]:.4g} ({r[q]:.3g}x)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("other", nargs="?", help="second capture to compare against")
    ap.add_argument("--huge", type=float, default=1e8, help="|x| at or above this counts as huge (default 1e8)")
    ap.add_argument("--ratio", type=float, default=1e3, help="compare: flag max |x| growth above this (default 1000)")
    ap.add_argument("--top", type=int, default=8, help="qwords listed per start pc")
    ap.add_argument("--control", help="TPC:QWORD negative control applied to the FIRST capture")
    args = ap.parse_args()

    control = None
    if args.control:
        t, q = args.control.split(":")
        control = (int(t, 0), int(q, 0))
        print(f"negative control: NaN written into qword {control[1]} lane x of every tpc {control[0]:#05x} run")

    a = scan(args.capture, args.huge, control)
    report(args.capture, *a, args.top)
    if args.other:
        b = scan(args.other, args.huge, None)
        report(args.other, *b, args.top)
        compare(args.capture, a[0], args.other, b[0], args.top, args.ratio)
    return 0


if __name__ == "__main__":
    sys.exit(main())
