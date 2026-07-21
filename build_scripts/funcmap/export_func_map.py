"""Export the function map the recompiler needs, from the IDA database.

WHY THIS EXISTS
---------------
ps2_recomp's `ghidra_output` key wants a CSV:

    name,start,end,size          <- header line, DISCARDED by the parser
    ADX_Init,0x0011f268,0x0011f3a0,0x138

elf_parser.cpp:1175-1181 splits each line on 4 commas. Anything that does not
split cleanly is skipped SILENTLY -- no warning, no error, exit code 0. That is
exactly how the earlier probe ran with zero boundary data while looking healthy.

The original export is gone alongside config.toml. SLUS_214.42 is STRIPPED
(.symtab size=0, .strtab size=0), so the names in output/ could only have come
from a Ghidra/IDA export. The .i64 still has them -- this recovers it.

These boundaries are ORIGINAL (IDA's own function analysis), not derived from
symbols.map's address list. That distinction matters: a symbols.map-derived map
would have to guess each `end` from the next symbol's start, which is wrong
wherever there is inter-function padding or a data island.

SAFETY
------
Read-only with respect to the database: closed with save=False. Writes exactly
one file, to the scratch dir. Touches nothing in the repo.

RUN
---
    python export_func_map.py

Requires IDA Professional 9.3's idalib (already importable on this machine).
The .i64 must not be open in the IDA GUI -- idalib cannot take the lock.
"""
import csv
import os
import sys

IDB = r"F:\SDBZ Recomp\ELF\SLUS_214.42.i64"
OUT = os.path.join(os.getcwd(), "sdbz_func_map.csv")

import idapro

print(f"opening {IDB} ...")
if idapro.open_database(IDB, run_auto_analysis=False) != 0:
    sys.exit("FAILED to open database (is it open in the IDA GUI?)")

import ida_funcs
import ida_name
import idautils

rows = []
skipped_empty = 0
sanitized = 0

try:
    for ea in idautils.Functions():
        f = ida_funcs.get_func(ea)
        if not f:
            continue
        start, end = f.start_ea, f.end_ea
        if end <= start:
            skipped_empty += 1
            continue

        # Prefer the demangled/user name; fall back to IDA's auto sub_XXXX.
        name = ida_name.get_ea_name(start, ida_name.GN_VISIBLE)
        if not name:
            name = f"sub_{start:08X}"

        # A comma in the name would shift every following CSV field and the
        # parser would silently drop the line. Same failure mode we just hit.
        if "," in name:
            name = name.replace(",", "_")
            sanitized += 1

        rows.append((name, f"0x{start:08x}", f"0x{end:08x}", f"0x{end - start:x}"))
finally:
    idapro.close_database(save=False)

rows.sort(key=lambda r: int(r[1], 16))

with open(OUT, "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["name", "start", "end", "size"])
    w.writerows(rows)

named = sum(1 for r in rows if not r[0].startswith("sub_"))
print()
print(f"wrote {OUT}")
print(f"  functions      : {len(rows)}")
print(f"  real names     : {named}")
print(f"  auto sub_ names: {len(rows) - named}")
if skipped_empty:
    print(f"  skipped (empty): {skipped_empty}")
if sanitized:
    print(f"  comma-sanitized: {sanitized}")
print()
if rows:
    print("first 5:")
    for r in rows[:5]:
        print("  " + ",".join(r))
