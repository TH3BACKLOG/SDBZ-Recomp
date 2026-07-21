"""Add the four static-init thunks IDA missed at 0x4dd500-0x4dd630.

WHY THESE FOUR
--------------
The initializer pointer table at 0x004e6c90 holds 86 consecutive code pointers.
82 of them are already defined functions in the merged map; map coverage begins
exactly at the 5th target (sub_4DD630). Only the first four targets fall in a
hole no function covers -- IDA's analysis never reached them, so the merged map
inherits the gap and the recompiler emits no dispatch slot for any of them.

BOUNDARIES ARE MEASURED, NOT ASSUMED
------------------------------------
Each one has the identical shape, read straight out of the ELF:

    lui/addiu/sw ...          argument setup
    j 0x00171f10              tail call to the shared registration routine
    addiu $a2, $a2, imm       delay slot
    nop [nop ...]             padding to the next 0x10 boundary

so `end` is the word after the delay slot, excluding trailing padding. That
matches the convention IDA uses for its own neighbours here (sub_4DE250 ends
0x4de444, not 0x4de450), so the added rows are consistent with the rest of the
file rather than a different measuring rule.

An earlier `jr $ra` scan found no return in any of them and flagged all four
SUSPECT. That scan was wrong for this shape: these are tail calls, they never
execute a return of their own. The `j` is the terminator.

Idempotent: refuses to add an address the map already covers.
"""
import csv
import os

MAP = os.path.join(os.getcwd(), "sdbz_func_map_merged.csv")
OUT = os.path.join(os.getcwd(), "sdbz_func_map_merged.csv")

# name, start, end(exclusive, padding excluded)
ADD = [
    ("static_init_thunk_4DD500", 0x004DD500, 0x004DD568),
    ("static_init_thunk_4DD570", 0x004DD570, 0x004DD5A4),
    ("static_init_thunk_4DD5B0", 0x004DD5B0, 0x004DD5D4),
    ("static_init_thunk_4DD5E0", 0x004DD5E0, 0x004DD62C),
]

rows = []
with open(MAP) as f:
    r = csv.reader(f)
    header = next(r)
    for name, s, e, z in r:
        rows.append([name, int(s, 16), int(e, 16), int(z, 16)])

existing = {r[1] for r in rows}
added = 0
for name, s, e in ADD:
    if s in existing:
        print(f"  SKIP 0x{s:08x} -- already in map")
        continue
    # refuse to overlap an existing function
    clash = [r for r in rows if r[1] < e and r[2] > s]
    if clash:
        print(f"  SKIP 0x{s:08x} -- would overlap {clash[0][0]} "
              f"0x{clash[0][1]:08x}-0x{clash[0][2]:08x}")
        continue
    rows.append([name, s, e, e - s])
    added += 1
    print(f"  ADD  {name},0x{s:08x},0x{e:08x},0x{e-s:x}")

rows.sort(key=lambda r: r[1])
with open(OUT, "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(header)
    for name, s, e, z in rows:
        w.writerow([name, f"0x{s:08x}", f"0x{e:08x}", f"0x{z:x}"])

print(f"\nadded {added}; map now {len(rows)} functions -> {OUT}")
