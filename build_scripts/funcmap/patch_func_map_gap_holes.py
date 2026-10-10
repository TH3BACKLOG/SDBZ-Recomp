"""Add 13 functions the merged map's IDA-derived boundary source missed.

WHY THESE THIRTEEN
-------------------
Session 2026-08-29: comparing the live `output/` tree (generated from the
now-lost original Ghidra export) against a scratch regen off the current
merged map by RAW FILENAME initially looked like ~19,234 missing functions.
Re-checked by ADDRESS COVERAGE instead (does each live-only address fall
inside some range in the merged map): 19,215 of those are not missing at all
-- they're the same functions, just carrying a real `sub_`/named entry in the
merged map instead of the old generic `fn_XXXXXX` name the live tree used.
Only 12 addresses were genuine gaps (not covered by any range).

One of the 12 is 0x0017EE80 -- the exact syscall-0x83 handler this session's
`registerFunction` fix in game_overrides.cpp depends on. Regenerating from the
un-patched map would silently drop its dispatch entry again.

BOUNDARIES ARE MEASURED, NOT ASSUMED
-------------------------------------
Each of the 12 addresses' enclosing gap (prev entry's end -> next entry's
start) was walked instruction-by-instruction from the ELF with
`mips_r5900_disassembler.py`, splitting functions at `jr $ra` + its delay
slot (the same terminator convention the rest of this map already uses).
Every walk landed EXACTLY on the next known entry's start, with at most a
few trailing `nop` padding words before it (same padding pattern documented
for the 4 `static_init_thunk_*` additions) -- no walk overran, underran, or
hit unreadable memory. That exactness across all 11 gaps (2 of the 12
addresses share one gap: 0x0017EE48/0x0017EE80) is the validation.

Idempotent: refuses to add an address the map already covers, and refuses
to overlap an existing function.
"""
import csv
import os

MAP = os.path.join(os.getcwd(), "sdbz_func_map_merged.csv")
OUT = os.path.join(os.getcwd(), "sdbz_func_map_merged.csv")

# name, start, end (exclusive, trailing nop padding excluded)
ADD = [
    ("sub_1137A4", 0x001137A4, 0x00113860),
    ("sub_17EE48", 0x0017EE48, 0x0017EE80),
    ("sub_17EE80", 0x0017EE80, 0x0017EEC0),
    ("sub_1A44D4", 0x001A44D4, 0x001A45A0),
    ("sub_1BF2D4", 0x001BF2D4, 0x001BF300),
    ("sub_1C997C", 0x001C997C, 0x001C9A74),
    ("sub_1C9AB0", 0x001C9AB0, 0x001C9BB8),
    ("sub_22C8E4", 0x0022C8E4, 0x0022C928),
    ("sub_22CBD8", 0x0022CBD8, 0x0022CC48),
    ("sub_256960", 0x00256960, 0x00257160),
    ("sub_257160", 0x00257160, 0x00257980),
    ("sub_341920", 0x00341920, 0x00341DB0),
    ("sub_356CD0", 0x00356CD0, 0x003571F4),
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
