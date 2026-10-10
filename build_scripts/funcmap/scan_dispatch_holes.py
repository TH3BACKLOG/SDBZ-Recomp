"""Static hunt for every code address the game can branch to that the BUILD cannot dispatch.

A hole = a guest address that something points at, but that has no slot in the
dispatch table. Hitting one at runtime prints `[guest-branch:missing-target]` and
the game freezes (0x293200, 0x27f530, 0x295be0 were all this).

Dispatchable set (what the built exe actually knows):
  * every `// 0x...` slot in ps2xRuntime/src/runner/register_functions.cpp
    (read as one file -- never list the runner dir)
  * every word of every kRecoveredFns range (game_overrides.cpp registers them all)
  * every `registerFunction(0x...)` literal under ps2xRuntime/src/lib

Reference sources:
  data  = an aligned data word (outside every code row) holding the address
          -> vtables, callback tables. Highest risk: reached by JALR.
  j/jal = direct jumps/calls inside code rows
  pair  = lui + addiu/ori building the address (function pointer arguments)

INTERIOR = inside a map row but no slot (jump-table cases, return addresses in
EH tables -- mostly noise); hidden unless --all.
Hole bodies are scanned too, so a recovered thunk's own `j` target is checked.

Atexit destructor stubs (body tail-jumps to the array dtor 0x171bb0) are tagged
`atexit` -- they only run when the game exits -- and bodies ending in `eret` are tagged
`eret` (kernel exception handlers installed via syscall; the runtime never vectors to them).
Both are hidden unless --all and never written to --csv.

RUN
---
    python build_scripts/funcmap/scan_dispatch_holes.py [--csv recovered_new.csv] [--all]

--csv writes the non-atexit holes as recovered_*.csv rows (end = next dispatchable
address / map row, trailing zero words trimmed), ready for apply_recovered_rows.py.
"""
import argparse
import bisect
import collections
import csv
import glob
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = os.path.join(ROOT, 'ELF', 'SLUS_214.42')
MAP = os.path.join(ROOT, 'build_scripts', 'funcmap', 'sdbz_func_map_merged.csv')
REG = os.path.join(ROOT, 'ps2xRuntime', 'src', 'runner', 'register_functions.cpp')
LIB = os.path.join(ROOT, 'ps2xRuntime', 'src', 'lib')
RECH = os.path.join(LIB, 'Kernel', 'recovered', 'recovered_functions.h')
BIAS = 0xFFF80                 # ELF vaddr = file offset + 0xFFF80
ARRAY_DTOR = 0x171bb0          # atexit stubs tail-jump here
JR_RA = 0x03E00008
ERET = 0x42000018               # bodies ending in eret are exception handlers (SetVTLBRefillHandler etc.)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--csv', help='write non-atexit holes here (recovered_*.csv format)')
    ap.add_argument('--all', action='store_true', help='also list atexit stubs')
    args = ap.parse_args()

    elf = open(ELF, 'rb').read()
    ins = lambda a: struct.unpack_from('<I', elf, a - BIAS)[0]

    rows = sorted((int(r['start'], 16), int(r['end'], 16)) for r in csv.DictReader(open(MAP)))
    rstarts = [s for s, _ in rows]
    tlo = rows[0][0]

    def in_row(a):
        i = bisect.bisect_right(rstarts, a) - 1
        return i >= 0 and rows[i][0] <= a < rows[i][1]

    slots = set()
    with open(REG, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'// 0x([0-9a-fA-F]+)\s*$', line)
            if m:
                slots.add(int(m.group(1), 16))
    thi = max(slots) + 4
    n_gen = len(slots)

    rec = [(int(a, 16), int(b, 16)) for a, b in re.findall(r'\{0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u, &', open(RECH).read())]
    for a, b in rec:
        slots.update(range(a, b, 4))
    n_ovr = 0
    for p in glob.glob(os.path.join(LIB, '**', '*.cpp'), recursive=True):
        for m in re.finditer(r'registerFunction\(\s*0x([0-9a-fA-F]+)', open(p, encoding='utf-8', errors='replace').read()):
            slots.add(int(m.group(1), 16)); n_ovr += 1
    print(f'dispatchable: {n_gen} generated slots, {len(rec)} recovered ranges, {n_ovr} override literals')

    # big unmapped spans inside the text range are rodata/strings, not code
    data_spans = []
    pe = tlo
    for s0, e0 in rows:
        if s0 - pe >= 0x4000:
            data_spans.append((pe, s0))
        pe = max(pe, e0)

    def in_data(t):
        return any(a0 <= t < b0 for a0, b0 in data_spans)

    def code_like(t):
        if not (tlo <= t < thi) or t & 3 or in_data(t):
            return False
        w = ins(t)
        if w == 0:
            return False
        # a function entry is the first word after padding / a return, or a mapped/recovered start
        p1, p2 = ins(t - 4), ins(t - 8)
        return p1 == 0 or p2 == JR_RA or (p1 >> 26) in (0x02,) or in_row(t)

    refs = collections.defaultdict(list)
    for off in range(0, len(elf) - 4, 4):
        va = off + BIAS
        if tlo <= va < thi and in_row(va):
            continue
        t = struct.unpack_from('<I', elf, off)[0]
        if t not in slots and code_like(t):
            refs[t].append(('data', va))
    def scan_code(s, e):
        hi = {}
        for a in range(s, e, 4):
            w = ins(a); op = w >> 26
            if op in (2, 3):
                t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if t not in slots and not in_row(t):
                    refs[t].append(('jal' if op == 3 else 'j', a))
            if op == 0x0F:
                hi[(w >> 16) & 31] = ((w & 0xFFFF) << 16, a)
            elif op in (0x09, 0x0D):
                rs = (w >> 21) & 31
                if rs in hi and a - hi[rs][1] <= 256:
                    imm = w & 0xFFFF
                    v = (hi[rs][0] + (imm - 0x10000 if (op == 9 and imm & 0x8000) else imm)) & 0xFFFFFFFF
                    if v not in slots and code_like(v) and not in_row(v):
                        refs[v].append(('pair', a))

    for s, e in rows:
        scan_code(s, e)

    rec_starts = {a for a, _ in rec}

    def bound(t):
        # up to the next map row / recovered body / other hole (NOT generated slots: stale
        # resume aliases of an older, wider row would cut the body short), trailing zero
        # words trimmed -- but a trailing `nop` that is a jump's delay slot stays.
        nxt = sorted_slots[bisect.bisect_right(sorted_slots, t)]
        e = nxt
        while e > t + 4 and ins(e - 4) == 0:
            e -= 4
        w = ins(e - 4)
        if e < nxt and ((w >> 26) in (2, 3) or (w & 0xFC00003E) == 0x00000008 or 4 <= (w >> 26) <= 7):
            e += 4   # jr/jalr, j/jal, beq/bne/blez/bgtz: keep the delay slot
        return e

    # a recovered hole body is code too: its own j/jal/pair targets must dispatch (transitive)
    scanned = set()
    while True:
        sorted_slots = sorted(set(rstarts) | rec_starts | set(refs))
        todo = [t for t in refs if t not in scanned and not in_row(t)]
        if not todo:
            break
        for t in todo:
            scanned.add(t)
            scan_code(t, bound(t))

    rank = {'data': 0, 'j': 1, 'jal': 1, 'pair': 2}
    out = []
    for t in refs:
        e = bound(t)
        body = [ins(a) for a in range(t, e, 4)]
        atexit = any((w >> 26) == 2 and ((w & 0x3FFFFFF) << 2) == ARRAY_DTOR for w in body)
        atexit = atexit or ERET in body   # kernel exception handler: the runtime never vectors to it
        inside = in_row(t)
        kinds = collections.Counter(k for k, _ in refs[t])
        best = min(rank[k] for k in kinds)
        out.append((atexit, best, t, e, inside, kinds, refs[t]))
    out.sort()
    shown = 0
    for atexit, best, t, e, inside, kinds, r in out:
        if (atexit or inside) and not args.all:
            continue
        tag = ('eret' if ERET in [ins(a) for a in range(t, e, 4)] else 'atexit') if atexit else ('INTERIOR' if inside else 'HOLE')
        print(f'{tag:8} 0x{t:06x}-0x{e:06x} len={e - t:#x} w0=0x{ins(t):08x} {dict(kinds)} at={[hex(v) for _, v in r[:3]]}')
        shown += 1
    n_at = sum(1 for o in out if o[0])
    n_in = sum(1 for o in out if o[4] and not o[0])
    print(f'{len(out)} undispatchable target(s): {len(out) - n_at - n_in} HOLE, {n_in} INTERIOR '
          f'(inside a map row, mostly jump-table/return-address noise; --all lists them), {n_at} atexit stubs')

    if args.csv:
        sel = [(t, e) for atexit, _, t, e, inside, _, _ in out if not atexit and not inside]
        sel.sort()
        s = 'name,start,end,size\n' + ''.join(f'sub_{a:08X},0x{a:08x},0x{b:08x},0x{b - a:x}\n' for a, b in sel)
        with open(args.csv + '.tmp', 'w', newline='') as f:
            f.write(s)
        os.replace(args.csv + '.tmp', args.csv)
        print('wrote', len(sel), 'rows ->', args.csv)


if __name__ == '__main__':
    main()
