"""Find code pointers in the ELF that land on a function nobody recompiled.

Every in-fight freeze so far (0x293200, 0x27f530, 0x295be0) was the same shape:
a vtable/callback word points at a real function prologue that sits in a gap of
sdbz_func_map_merged.csv, so the JALR hits `[guest-branch:missing-target]`.

This walks every aligned word of the ELF, keeps values that point into code,
are not inside any func-map row, and land on `addiu $sp, $sp, -x` -- and, for
each, derives the body end (first `jr $ra` whose delay slot pops the same frame).
Output is a CSV in recovered_*.csv format, ready for apply_recovered_rows.py.

A target that already has a body in Kernel/recovered/ is reported but skipped.

RUN
---
    python build_scripts/funcmap/scan_vtable_holes.py [--out recovered_new.csv]
"""
import argparse
import bisect
import csv
import glob
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = os.path.join(ROOT, 'ELF', 'SLUS_214.42')
MAP = os.path.join(ROOT, 'build_scripts', 'funcmap', 'sdbz_func_map_merged.csv')
RECOVERED = os.path.join(ROOT, 'ps2xRuntime', 'src', 'lib', 'Kernel', 'recovered')
VADDR_BIAS = 0xFFF80          # ELF vaddr = file offset + 0xFFF80
CODE_LO, CODE_HI = 0x100000, 0x480000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', help='write new rows here (recovered_*.csv format)')
    args = ap.parse_args()

    elf = open(ELF, 'rb').read()
    rows = sorted((int(r['start'], 16), int(r['end'], 16)) for r in csv.DictReader(open(MAP)))
    starts = [s for s, _ in rows]

    def inside(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and rows[i][0] <= a < rows[i][1]

    def ins(va):
        return struct.unpack_from('<I', elf, va - VADDR_BIAS)[0]

    have = set()
    for f in glob.glob(os.path.join(RECOVERED, '*.cpp')):
        m = re.search(r'_0x([0-9a-fA-F]+)\.cpp$', f)
        if m:
            have.add(int(m.group(1), 16))

    refs = {}
    for off in range(0, len(elf) - 4, 4):
        t = struct.unpack_from('<I', elf, off)[0]
        if CODE_LO <= t < CODE_HI and t % 4 == 0 and not inside(t):
            if (ins(t) >> 16) == 0x27BD and (ins(t) & 0x8000):
                refs.setdefault(t, []).append(off + VADDR_BIAS)

    new = []
    for t in sorted(refs):
        nxt = starts[bisect.bisect_right(starts, t)]
        frame = 0x10000 - (ins(t) & 0xFFFF)
        end = None
        a = t
        while a < nxt:
            if ins(a) == 0x03E00008:
                d = ins(a + 4)
                if (d >> 16) == 0x27BD and (d & 0xFFFF) == frame:
                    end = a + 8
                    break
            a += 4
        tag = 'HAS BODY' if t in have else ('NO END FOUND' if end is None else 'NEW')
        print(f'0x{t:06x}  end={hex(end) if end else "?"}  next_mapped=0x{nxt:06x}  {tag}  refs={[hex(r) for r in refs[t][:3]]}')
        if tag == 'NEW':
            new.append((t, end))

    print(f'{len(refs)} unmapped prologue target(s), {len(new)} new')
    if args.out and new:
        s = 'name,start,end,size\n' + ''.join(
            f'sub_{a:08X},0x{a:08x},0x{b:08x},0x{b - a:x}\n' for a, b in new)
        with open(args.out + '.tmp', 'w', newline='') as f:
            f.write(s)
        os.replace(args.out + '.tmp', args.out)
        print('wrote', args.out)


if __name__ == '__main__':
    main()
