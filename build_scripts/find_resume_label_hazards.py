#!/usr/bin/env python3
"""
find_resume_label_hazards.py -- enumerate the ENTIRE mid-function-resume bug class
statically, instead of discovering it one freeze at a time.

Background
----------
The generator emits, at the top of most recompiled functions:

    switch (ctx->pc) { case 0xNNNN: goto label_NNNN; }

turning interior addresses into entry points so a thread can resume after a
syscall/JAL boundary.  Entering there SKIPS THE PROLOGUE.  For a function whose
prologue does real work, that is not a resume -- it is a corrupt half-execution.

Two independent hazards, both decidable from the emitted source:

  SP_LEAK   a resume label sits AFTER `addiu $sp,$sp,-N` but BEFORE the matching
            `addiu $sp,$sp,+N`.  The frame is never allocated, but it IS
            deallocated, so $sp climbs by N on every such entry.  A function that
            returns must have zero net sp delta across a dispatch -- so this is a
            hard, universal invariant, not a heuristic.

  RA_STALE  a resume label sits AFTER `sd $ra,off($sp)` but AT/BEFORE the matching
            `ld $ra,off($sp)`.  The store never ran this invocation, so the load
            pulls whatever is live at that address -- foreign frame, or zero.
            `jr $ra` then jumps there.  Zero => the pc==0 dormant freeze.

Both fire together in noop_wrapper_m_0x177fe8, the part-43/44 freeze.

Usage
-----
    python build_scripts/find_resume_label_hazards.py output
    python build_scripts/find_resume_label_hazards.py output --csv hazards.csv
    python build_scripts/find_resume_label_hazards.py output --only BOTH
"""
import argparse
import csv
import os
import re
import sys
from collections import Counter

RE_CASE = re.compile(r'case\s+0x([0-9a-fA-F]+)u?\s*:\s*goto\s+label_', re.I)
RE_INSN = re.compile(
    r'^\s*//\s*0x([0-9a-fA-F]+):\s*0x[0-9a-fA-F]+\s+(\S+)\s*(.*?)\s*$')
RE_SPADJ = re.compile(r'\$sp\s*,\s*\$sp\s*,\s*(-?)0x([0-9a-fA-F]+)', re.I)
RE_RAMEM = re.compile(r'\$ra\s*,\s*(-?)0x([0-9a-fA-F]+)\s*\(\s*\$sp\s*\)', re.I)


def parse(path):
    """Return (labels, spdown, spup, sdra, ldra, jrra) as addr lists."""
    labels, spdown, spup, sdra, ldra, jrra = [], [], [], [], [], []
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            m = RE_CASE.search(line)
            if m:
                labels.append(int(m.group(1), 16))
                continue
            m = RE_INSN.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            mn = m.group(2).lower()
            ops = m.group(3)
            if mn in ('addiu', 'daddiu') and '$sp' in ops:
                sp = RE_SPADJ.search(ops)
                if sp:
                    val = int(sp.group(2), 16)
                    (spdown if sp.group(1) == '-' else spup).append((addr, val))
            elif mn in ('sd', 'sw', 'sq') and RE_RAMEM.search(ops):
                sdra.append((addr, int(RE_RAMEM.search(ops).group(2), 16)))
            elif mn in ('ld', 'lw', 'lq') and RE_RAMEM.search(ops):
                ldra.append((addr, int(RE_RAMEM.search(ops).group(2), 16)))
            elif mn == 'jr' and '$ra' in ops:
                jrra.append(addr)
    return labels, spdown, spup, sdra, ldra, jrra


def classify(labels, spdown, spup, sdra, ldra, jrra):
    """Per-label hazard set. Returns list of (label, kinds, detail)."""
    out = []
    for lb in sorted(set(labels)):
        kinds = []
        detail = []
        # SP_LEAK: an allocation strictly before the label, a matching
        # deallocation at/after it -> the frame is freed but never made.
        for (a, n) in spdown:
            if a < lb and any(u >= lb and un == n for (u, un) in spup):
                kinds.append('SP_LEAK')
                detail.append('sp-%#x@%#x freed@>=%#x' % (n, a, lb))
                break
        # RA_STALE: $ra store strictly before the label, reload at/after it,
        # same frame offset, and a jr $ra at/after the label to consume it.
        for (a, off) in sdra:
            if a >= lb:
                continue
            rl = [l for (l, o) in ldra if l >= lb and o == off]
            if rl and any(j >= rl[0] for j in jrra):
                kinds.append('RA_STALE')
                detail.append('sd$ra+%#x@%#x reload@%#x' % (off, a, rl[0]))
                break
        if kinds:
            out.append((lb, kinds, '; '.join(detail)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root', help='directory of generated .cpp')
    ap.add_argument('--csv', help='write full findings here')
    ap.add_argument('--only', choices=['SP_LEAK', 'RA_STALE', 'BOTH'],
                    help='restrict the printed list')
    ap.add_argument('--limit', type=int, default=25)
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.root) if f.endswith('.cpp'))
    rows = []
    stats = Counter()
    for i, name in enumerate(files):
        if i % 2000 == 0:
            print('  ...%d/%d' % (i, len(files)), file=sys.stderr)
        parsed = parse(os.path.join(args.root, name))
        if not parsed[0]:
            stats['no_resume_label'] += 1
            continue
        stats['has_resume_label'] += 1
        haz = classify(*parsed)
        if not haz:
            stats['resume_label_safe'] += 1
            continue
        stats['func_hazardous'] += 1
        for (lb, kinds, detail) in haz:
            tag = 'BOTH' if len(kinds) == 2 else kinds[0]
            stats['label_' + tag] += 1
            rows.append((name, '0x%x' % lb, tag, detail))

    print('\n=== scanned %d files in %s ===' % (len(files), args.root))
    for k in ('no_resume_label', 'has_resume_label', 'resume_label_safe',
              'func_hazardous', 'label_SP_LEAK', 'label_RA_STALE', 'label_BOTH'):
        print('  %-20s %d' % (k, stats[k]))

    sel = [r for r in rows if not args.only or r[2] == args.only]
    sel.sort(key=lambda r: (r[2] != 'BOTH', r[0]))
    print('\n--- %d hazardous labels%s (showing %d) ---'
          % (len(sel), ' [%s]' % args.only if args.only else '',
             min(args.limit, len(sel))))
    for r in sel[:args.limit]:
        print('  %-44s %-10s %-9s %s' % (r[0], r[1], r[2], r[3]))

    if args.csv:
        with open(args.csv, 'w', newline='', encoding='utf-8') as fh:
            w = csv.writer(fh)
            w.writerow(['file', 'label', 'hazard', 'detail'])
            w.writerows(rows)
        print('\nwrote %s (%d rows)' % (args.csv, len(rows)))


if __name__ == '__main__':
    main()
