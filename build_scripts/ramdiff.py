"""ramdiff.py - find the RAM words behind a menu cursor / stage id from `snap` dumps.

The pad script op `snap <name>` (ps2_pad.cpp) writes all 32 MB of EE RAM to
gsdump/<run>/snap/<name>.bin (address = file offset). Take one snapshot per cursor
position, plus a NOISE pair (two snapshots with no input between them), then:

  python build_scripts/ramdiff.py gsdump/disc1/snap --steps st0 st1 st2 st3 --noise n0 n1 [--max 255]
      -> addresses whose value changes at EVERY step and is identical in the noise pair
         (frame counters etc. differ in the noise pair and are dropped). Ranked: constant
         delta first (a cursor index), then smallest values.
  python build_scripts/ramdiff.py gsdump/disc1/snap --at 0x5b4070 st0 st1 st2
      -> the byte/half/word at one address in each named snapshot.

Labels: output is CANDIDATES. A cursor index is confirmed only by poking it (script `poke`)
and seeing the screen change.
"""
import argparse
import os
import sys

import numpy as np


def load(d, name):
    p = os.path.join(d, name + '.bin')
    if not os.path.isfile(p):
        sys.exit('missing snapshot: ' + p)
    return np.fromfile(p, dtype=np.uint8)


def view(a, w):
    n = (len(a) // w) * w
    return a[:n].view({1: np.uint8, 2: '<u2', 4: '<u4'}[w])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--steps', nargs='+')
    ap.add_argument('--noise', nargs=2)
    ap.add_argument('--max', type=lambda x: int(x, 0), default=255, help='largest value allowed at any step')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--lo', type=lambda x: int(x, 0), default=0x100000)
    ap.add_argument('--hi', type=lambda x: int(x, 0), default=0x2000000)
    ap.add_argument('--at', type=lambda x: int(x, 0))
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()

    if a.at is not None:
        for n in a.names:
            r = load(a.dir, n)
            ad = a.at
            print('%-12s u8=%3d u16=%5d u32=%10d' % (n, r[ad], int(view(r[ad & ~1:], 2)[0]) if ad < len(r) - 2 else -1,
                  int(view(r[ad & ~3:], 4)[0]) if ad < len(r) - 4 else -1))
        return
    if not a.steps or len(a.steps) < 2:
        sys.exit('need --steps with at least 2 snapshots')

    snaps = [load(a.dir, n) for n in a.steps]
    noise = [load(a.dir, n) for n in a.noise] if a.noise else None
    if not noise:
        print('WARNING: no --noise pair; frame counters will be listed too')
    for w in (1, 2, 4):
        vs = [view(s, w) for s in snaps]
        m = np.ones(len(vs[0]), dtype=bool)
        for i in range(len(vs) - 1):
            m &= vs[i] != vs[i + 1]
        if noise:
            m &= view(noise[0], w) == view(noise[1], w)
        for v in vs:
            m &= v <= a.max
        lo, hi = a.lo // w, a.hi // w
        idx = np.nonzero(m)[0]
        idx = idx[(idx >= lo) & (idx < hi)]
        rows = []
        mod = 1 << (8 * w)
        for k in idx:
            series = [int(v[k]) for v in vs]
            d = {(series[i + 1] - series[i]) % mod for i in range(len(series) - 1)}
            rows.append((0 if len(d) == 1 else 1, max(series), int(k) * w, series))
        rows.sort()
        print('== width %d: %d candidate(s)%s' % (w, len(rows), '' if len(rows) <= a.top else ' (top %d)' % a.top))
        for const, mx, ad, series in rows[:a.top]:
            print('  0x%06X  %s%s' % (ad, series, '  const-delta' if const == 0 else ''))


if __name__ == '__main__':
    main()
