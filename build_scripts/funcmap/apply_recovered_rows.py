"""Merge recovered function rows into sdbz_func_map_merged.csv.

Generalizes patch_func_map.py (which hardcodes its four static-init thunks and
stands as the written record of why those four are correct) to any CSV of rows
produced by recover_hole_boundaries.py.

Same three guards as the original, because each has already caught something:

  * idempotent   -- an address already in the map is skipped, so re-running is
                    safe and does not duplicate rows.
  * no overlap   -- a row intersecting an existing range is refused. An
                    overlapping row would make the recompiler re-slice a body
                    that currently works.
  * count assert -- elf_parser.cpp skips malformed rows SILENTLY and exits 0
                    (that is how an earlier probe ran with no boundary data
                    while looking healthy). So the map is re-read from disk
                    after writing and the row count must have grown by exactly
                    the number added, or this exits non-zero.

A timestamped .bak of the map is written before the first modification.

RUN
---
    python build_scripts/funcmap/apply_recovered_rows.py \
        --add build_scripts/funcmap/recovered_holes.csv
"""
import argparse
import csv
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MAP = os.path.join(HERE, "sdbz_func_map_merged.csv")


def read_map(path):
    with open(path, newline="") as fh:
        r = csv.reader(fh)
        header = next(r)
        rows = []
        for row in r:
            if len(row) != 4:
                continue
            rows.append([row[0], int(row[1], 16), int(row[2], 16), int(row[3], 16)])
    return header, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", default=DEFAULT_MAP)
    ap.add_argument("--add", required=True, help="CSV of rows to merge in")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    header, rows = read_map(args.map)
    _, incoming = read_map(args.add)
    before = len(rows)
    print(f"map: {before} rows; incoming: {len(incoming)} rows")

    existing_starts = {r[1] for r in rows}
    added, skipped, clashed = [], 0, 0

    for name, s, e, _z in incoming:
        if s in existing_starts:
            skipped += 1
            continue
        clash = next((r for r in rows if r[1] < e and r[2] > s), None)
        if clash:
            clashed += 1
            print(f"  SKIP 0x{s:08x}-0x{e:08x} -- overlaps {clash[0]} "
                  f"0x{clash[1]:08x}-0x{clash[2]:08x}")
            continue
        rows.append([name, s, e, e - s])
        existing_starts.add(s)
        added.append(s)

    print(f"\nto add: {len(added)}   already present: {skipped}   refused (overlap): {clashed}")
    if args.dry_run:
        print("dry run -- map not written")
        return
    if not added:
        print("nothing to do; map unchanged")
        return

    bak = f"{args.map}.{time.strftime('%Y%m%d-%H%M%S')}.bak"
    shutil.copy2(args.map, bak)
    print(f"backup -> {bak}")

    rows.sort(key=lambda r: r[1])
    with open(args.map, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        for name, s, e, z in rows:
            w.writerow([name, f"0x{s:08x}", f"0x{e:08x}", f"0x{z:x}"])

    # Re-read from disk: the only check that proves the rows actually parse.
    _, after_rows = read_map(args.map)
    expected = before + len(added)
    print(f"\nre-read {args.map}: {len(after_rows)} rows (expected {expected})")
    if len(after_rows) != expected:
        sys.exit(f"FAIL: row count is {len(after_rows)}, expected {expected} -- "
                 "some rows did not survive the write/parse round trip")

    missing = [s for s in added if not any(r[1] == s for r in after_rows)]
    if missing:
        sys.exit(f"FAIL: {len(missing)} added starts absent after re-read, "
                 f"first 0x{missing[0]:08x}")
    print("OK: every added row round-tripped")


if __name__ == "__main__":
    main()
