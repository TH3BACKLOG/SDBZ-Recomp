"""Find direct-branch targets that have no recompiled dispatch slot.

This is the static form of the runtime fault:

    [guest-branch:missing-target] kind=DirectJump op=J source=0xAAAA target=0xBBBB

Under MissingFunctionPolicy::ContinueToTarget the runtime logs that once, sets
pc, and returns *true* -- so the caller's generated body runs its own trailing
`ctx->pc = <next instruction>`. When that next instruction is inter-function
padding with no slot of its own, the dispatch loop misses on it forever. The
spin PC you see in the console is therefore one word past a func-map END; the
real hole is somewhere else entirely. This script finds the holes without
having to burn a run cycle on each one.

Every `j` / `jal` in the ELF is decoded, keeping only those whose *source*
lies inside a function the map knows about (anything else is a data word being
misread as an instruction). A target is a hole when no entry in
register_functions.cpp claims that exact address -- that table, not the CSV, is
what hasFunction() actually consults.

Targets are split into two classes, which need different fixes:

  missing-body  the target sits in a func-map gap, so no body was ever
                generated. Needs a hand-written override in game_overrides.cpp
                plus registerFunction(), or a regenerated func map.
  missing-slot  a body covers the target's address but nothing registers that
                entry point. Only reachable if something enters mid-function.

Usage:
    python build_scripts/find_dispatch_holes.py [--class missing-body]
"""

import argparse
import bisect
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_r5900_disassembler import load_elf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ELF = os.path.join(ROOT, "ELF", "SLUS_214.42")
DEFAULT_CSV = os.path.join(ROOT, "build_scripts", "funcmap", "sdbz_func_map_merged.csv")
DEFAULT_REG = os.path.join(ROOT, "ps2xRuntime", "src", "runner", "register_functions.cpp")
DEFAULT_OVERRIDES = os.path.join(ROOT, "ps2xRuntime", "src", "lib", "game_overrides.cpp")

# EE code lives well inside this window; targets outside it are decode noise.
CODE_LO, CODE_HI = 0x00100000, 0x00500000

SLOT_RE = re.compile(
    r"g_ps2RecompiledFunctionTable\[\d+\]\s*=\s*\w+;\s*//\s*0x([0-9A-Fa-f]+)"
)

# Overrides claim their address at runtime instead of via the generated table,
# so a hole already fixed in game_overrides.cpp is invisible to SLOT_RE and
# keeps inflating the missing-body count. Same address-keyed matching rationale
# as check_missing_functions.py: key on the constant, not the symbol name.
OVERRIDE_RE = re.compile(
    r"registerFunction\s*\(\s*0x([0-9A-Fa-f]+)[uU]?[lL]*\s*,"
)

# The recovered tail-chunk bodies (Stage 5.14) are registered by a loop over
# kRecoveredFns, not by 135 literal registerFunction() calls, so OVERRIDE_RE
# cannot see them. Without this the whole batch reads as missing-slot: they have
# bodies now, but nothing the scanner recognises claims their entry point.
DEFAULT_RECOVERED = os.path.join(
    ROOT, "ps2xRuntime", "src", "lib", "Kernel", "recovered", "recovered_functions.h"
)
RECOVERED_RE = re.compile(r"\{\s*0x([0-9A-Fa-f]+)[uU]?[lL]*\s*,\s*&\w+\s*\}")


def load_registered(path):
    """Guest addresses with a real g_ps2RecompiledFunctionTable slot."""
    addrs = set()
    with open(path, "r", errors="ignore") as fh:
        for line in fh:
            match = SLOT_RE.search(line)
            if match:
                addrs.add(int(match.group(1), 16))
    return addrs


def load_overrides(path):
    """Guest addresses claimed by runtime.registerFunction() in game_overrides.cpp.

    Commented-out registrations are skipped -- a `//`-disabled line does not
    claim the address, and counting it would hide a live hole.
    """
    addrs = set()
    if not path or not os.path.exists(path):
        return addrs
    with open(path, "r", errors="ignore") as fh:
        for line in fh:
            if line.lstrip().startswith("//"):
                continue
            for match in OVERRIDE_RE.finditer(line):
                addrs.add(int(match.group(1), 16))
    return addrs


def load_recovered(path):
    """Guest addresses in the kRecoveredFns table, registered by the loop in
    game_overrides.cpp. Only counted if that loop is actually present -- the
    header alone declares bodies, it does not claim addresses."""
    addrs = set()
    if not path or not os.path.exists(path):
        return addrs
    overrides = os.path.join(ROOT, "ps2xRuntime", "src", "lib", "game_overrides.cpp")
    if not os.path.exists(overrides):
        return addrs
    with open(overrides, errors="ignore") as fh:
        if "kRecoveredFns" not in fh.read():
            return addrs
    with open(path, errors="ignore") as fh:
        for line in fh:
            if line.lstrip().startswith("//"):
                continue
            for match in RECOVERED_RE.finditer(line):
                addrs.add(int(match.group(1), 16))
    return addrs


def load_ranges(path):
    ranges = []
    with open(path, newline="") as fh:
        for row in csv.reader(fh):
            try:
                ranges.append((int(row[1], 0), int(row[2], 0), row[0]))
            except (ValueError, IndexError):
                continue  # header and malformed rows
    ranges.sort()
    return ranges


def make_owner_lookup(ranges):
    starts = [r[0] for r in ranges]

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and ranges[i][0] <= addr < ranges[i][1]:
            return ranges[i]
        return None

    return owner


def scan(elf_path, csv_path, reg_path, overrides_path=None, recovered_path=None):
    registered = (load_registered(reg_path)
                  | load_overrides(overrides_path)
                  | load_recovered(recovered_path))
    owner = make_owner_lookup(load_ranges(csv_path))
    segments, _entry = load_elf(elf_path)

    holes = {}
    for vaddr, data in segments:
        for off in range(0, len(data) - 3, 4):
            word = int.from_bytes(data[off:off + 4], "little")
            op = word >> 26
            if op not in (2, 3):  # j, jal
                continue
            pc = vaddr + off
            if owner(pc) is None:
                continue
            target = (pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if not (CODE_LO <= target < CODE_HI) or target in registered:
                continue
            entry = holes.setdefault(
                target,
                {"sites": [], "kind": "missing-slot" if owner(target) else "missing-body"},
            )
            entry["sites"].append((pc, "jal" if op == 3 else "j"))
    return holes


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--funcmap", default=DEFAULT_CSV)
    ap.add_argument("--register-functions", default=DEFAULT_REG)
    ap.add_argument("--overrides", default=DEFAULT_OVERRIDES,
                    help="game_overrides.cpp; its registerFunction() addresses "
                         "count as registered. Pass '' to ignore them.")
    ap.add_argument("--recovered", default=DEFAULT_RECOVERED,
                    help="recovered_functions.h; its kRecoveredFns addresses count "
                         "as registered. Pass '' to ignore them.")
    ap.add_argument("--class", dest="want", choices=("missing-body", "missing-slot", "all"),
                    default="all", help="restrict output to one hole class")
    ap.add_argument("--addrs-only", action="store_true",
                    help="print bare 0x addresses only, for feeding other tools")
    args = ap.parse_args()

    holes = scan(args.elf, args.funcmap, args.register_functions, args.overrides,
                 args.recovered)

    shown = 0
    for target in sorted(holes):
        entry = holes[target]
        if args.want != "all" and entry["kind"] != args.want:
            continue
        shown += 1
        if args.addrs_only:
            print(f"0x{target:08x}")
            continue
        sites = entry["sites"]
        kinds = "/".join(sorted({k for _, k in sites}))
        srcs = ", ".join(f"0x{pc:x}" for pc, _ in sites[:5])
        more = "" if len(sites) <= 5 else f" (+{len(sites) - 5} more)"
        print(f"0x{target:08x}  {entry['kind']:<12} n={len(sites):<3} {kinds:<4} "
              f"from {srcs}{more}")

    if args.addrs_only:
        return
    bodies = sum(1 for h in holes.values() if h["kind"] == "missing-body")
    print(f"\n{shown} shown; {len(holes)} holes total "
          f"({bodies} missing-body, {len(holes) - bodies} missing-slot), "
          f"{sum(len(h['sites']) for h in holes.values())} call sites")


if __name__ == "__main__":
    main()
