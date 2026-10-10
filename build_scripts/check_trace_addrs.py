#!/usr/bin/env python3
"""Classify a list of EE addresses against the merged func map.

Given addresses harvested from a PCSX2 backtrace (ground truth: these are
functions the real hardware actually executes), report for each whether it is:

  START   - an exact func-map row start  -> the recompiler emits fn_<addr>
  FOLDED  - inside a row but not its start -> missing-slot hole (body exists
            as label_<addr> inside a sibling fn, but no dispatch entry)
  ABSENT  - in no row at all -> missing-body hole

Usage:
  python check_trace_addrs.py 0x14f880 0x14f1e0 ...
  python check_trace_addrs.py --file addrs.txt
"""
import argparse
import bisect
import csv
import pathlib
import re
import sys

CSV = pathlib.Path(__file__).parent / "funcmap" / "sdbz_func_map_merged.csv"
OVERRIDES = (
    pathlib.Path(__file__).parent.parent
    / "ps2xRuntime"
    / "src"
    / "lib"
    / "game_overrides.cpp"
)
OVERRIDE_RE = re.compile(r"registerFunction\s*\(\s*(0x[0-9a-fA-F]+)", re.I)


def load_map():
    rows = []
    with CSV.open(newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            try:
                rows.append(
                    (int(r["start"], 16), int(r["end"], 16), r["name"])
                )
            except (KeyError, ValueError):
                continue
    rows.sort()
    return rows


def load_overrides():
    if not OVERRIDES.exists():
        return set()
    text = OVERRIDES.read_text(encoding="utf-8", errors="replace")
    return {int(m.group(1), 16) for m in OVERRIDE_RE.finditer(text)}


def classify(addr, rows, starts, overrides):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return "ABSENT", None
    start, end, name = rows[i]
    if addr == start:
        return "START", name
    if addr < end:
        return "FOLDED", f"{name} @ 0x{start:08x}-0x{end:08x}"
    return "ABSENT", None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addrs", nargs="*")
    ap.add_argument("--file")
    args = ap.parse_args()

    text = " ".join(args.addrs)
    if args.file:
        text += " " + pathlib.Path(args.file).read_text(encoding="utf-8")
    addrs = [int(a, 16) for a in re.findall(r"0x[0-9a-fA-F]+", text)]
    if not addrs:
        print("no addresses given", file=sys.stderr)
        return 2

    rows = load_map()
    starts = [r[0] for r in rows]
    overrides = load_overrides()

    holes = 0
    for a in addrs:
        kind, detail = classify(a, rows, starts, overrides)
        ov = " [OVERRIDDEN]" if a in overrides else ""
        if kind != "START" and not ov:
            holes += 1
        print(f"0x{a:08x}  {kind:6}{ov}  {detail or ''}")

    print(f"\n{len(addrs)} addresses, {holes} unresolved hole(s), "
          f"{len(overrides)} overrides registered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
