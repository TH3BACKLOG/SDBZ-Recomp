#!/usr/bin/env python3
"""
decompile_missing.py

Headless (idalib) batch dump of every function in an IDA database that still
has an auto-generated name (sub_*, unk_*, loc_*, j_sub_* etc.) — i.e. every
function nobody has manually labeled yet. Dumps decompiled (Hex-Rays) or
disassembled bodies to a single flat text file for one-shot reading/labeling,
same pattern as irx_xref_dump.py.

Run manually, no Claude/MCP involved:
    <IDA install dir>\\python.exe build_scripts\\decompile_missing.py --db "<path>\\Module.i64"

Requires the IDA GUI/MCP bridge to be CLOSED first — idalib needs exclusive
access to the database file.

Options:
    --db        Path to the .i64/.idb database to open (required)
    --out       Output file path (default: build_scripts/decompile_missing_output.txt)
    --prefixes  Comma-separated list of auto-name prefixes to treat as
                "missing" (default: sub_,unk_,loc_,j_sub_,nullsub_)
    --limit     Max number of functions to dump (default: no limit)

Note: import idapro FIRST, before any other ida_* module (idalib requirement).
"""

import argparse
import os
import sys

import idapro  # must be imported first

import ida_funcs
import ida_idaapi
import ida_lines

BADADDR = ida_idaapi.BADADDR

try:
    import ida_hexrays
    HAVE_HEXRAYS = True
except ImportError:
    HAVE_HEXRAYS = False

DEFAULT_PREFIXES = ["sub_", "unk_", "loc_", "j_sub_", "nullsub_"]


def is_missing_name(name, prefixes):
    return any(name.startswith(p) for p in prefixes)


def render_function(fea):
    name = ida_funcs.get_func_name(fea) or hex(fea)
    lines = [f"--- function {name} @ {hex(fea)} ---"]
    if HAVE_HEXRAYS:
        try:
            cfunc = ida_hexrays.decompile(fea)
            if cfunc is not None:
                lines.append(str(cfunc))
                return "\n".join(lines)
        except Exception as e:
            lines.append(f"(decompile failed: {e}, falling back to disasm)")
    f = ida_funcs.get_func(fea)
    if f is None:
        lines.append("(no function object)")
        return "\n".join(lines)
    import ida_bytes
    ea = f.start_ea
    while ea < f.end_ea:
        disasm = ida_lines.generate_disasm_line(ea, 0) or ""
        clean = ida_lines.tag_remove(disasm)
        lines.append(f"    {hex(ea)}: {clean}")
        ea = ida_bytes.next_head(ea, f.end_ea)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", required=True, help="Path to .i64/.idb database")
    parser.add_argument("--out", default=None, help="Output file path")
    parser.add_argument("--prefixes", default=None,
                         help="Comma-separated auto-name prefixes counted as 'missing'")
    parser.add_argument("--limit", type=int, default=None, help="Max functions to dump")
    args = parser.parse_args()

    prefixes = DEFAULT_PREFIXES if args.prefixes is None else [p.strip() for p in args.prefixes.split(",") if p.strip()]
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_path = args.out or os.path.join(script_dir, "decompile_missing_output.txt")

    print(f"Opening database {args.db}...")
    if idapro.open_database(args.db, True) != 0:
        print("ERROR: failed to open database")
        sys.exit(1)

    out_lines = []
    count = 0
    total = 0
    try:
        qty = ida_funcs.get_func_qty()
        for i in range(qty):
            f = ida_funcs.getn_func(i)
            if f is None:
                continue
            fea = f.start_ea
            name = ida_funcs.get_func_name(fea) or ""
            total += 1
            if not is_missing_name(name, prefixes):
                continue
            if args.limit is not None and count >= args.limit:
                break
            out_lines.append(render_function(fea))
            out_lines.append("")
            count += 1
    finally:
        print("Closing database...")
        idapro.close_database(False)  # discard changes, this is read-only analysis

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out_lines))

    print(f"Scanned {total} functions, dumped {count} unlabeled ones.")
    print(f"Done. Wrote {out_path}")


if __name__ == "__main__":
    main()
