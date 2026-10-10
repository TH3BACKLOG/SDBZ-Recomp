#!/usr/bin/env python3
"""Pre-build check for orphaned call targets (BUG-008 analyzer fold pattern).

Scans runner/*.cpp for direct calls to fn_/sub_ symbols that have no
corresponding definition anywhere (runner/, fn_forward_decls.h, game_overrides.cpp).
For each orphan, checks whether it's actually a label folded inside a sibling
function's body (label_<addr>: present in some runner file) -- if so, prints
the exact extract_folded_function.py command to fix it. Otherwise flags it as
a genuinely missing function body (different bug class).

Usage:
    python build_scripts/check_missing_functions.py
    python build_scripts/check_missing_functions.py --shard 1 --of 8
    python build_scripts/check_missing_functions.py --rebuild-cache

--shard/--of splits the runner/*.cpp file list into N equal slices (1-indexed)
so the scan can be run manually in parallel PowerShell windows instead of one
long pass. The "defined" symbol set is always global (see cache below), so
sharding only affects which files are checked for *references* -- it no
longer produces cross-shard false positives.

Defined-symbol cache: the first run (or --rebuild-cache) does one full pass
over all runner/*.cpp files collecting ONLY definitions (cheap: single regex,
no label/reference bookkeeping) and writes them to
build_scripts/.defined_symbols_cache.txt. Every subsequent run (sharded or
not) loads that cache instead of rescanning files outside its own shard.
Delete the cache file or pass --rebuild-cache after the runner/ tree changes
(e.g. after running extract_folded_function.py) to pick up new definitions.

Output: full orphan list is always written to
build_scripts/.orphan_report.txt; the terminal only prints a summary plus the
first 30 entries so a large hit count doesn't flood the console.

Exit code 0 = no orphans found, 1 = orphans found (does not fail the build by itself).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
# NOTE: this script predates the repo layout it now lives in. It was restored
# from aac0d955, where every path carried a leading "PS2Recomp" component that
# does not exist in this tree -- so RUNNER_DIR never resolved, main() bailed at
# the is_dir() guard, and build.ps1 printed "Orphaned symbols found" on every
# single build without having scanned anything. Paths are repo-relative now.
RUNNER_DIR = ROOT / "ps2xRuntime" / "src" / "runner"
FORWARD_DECLS = ROOT / "ps2xRuntime" / "include" / "fn_forward_decls.h"
OVERRIDES = ROOT / "ps2xRuntime" / "src" / "lib" / "game_overrides.cpp"
# Recovered tail-chunk bodies (Stage 5.14). They define real fn_/sub_ symbols
# but live outside runner/ on purpose, so they must be counted as DEFINED or
# every one of them is reported as a missing call target.
RECOVERED_DIR = ROOT / "ps2xRuntime" / "src" / "lib" / "Kernel" / "recovered"
CACHE_FILE = Path(__file__).parent / ".defined_symbols_cache.txt"
REPORT_FILE = Path(__file__).parent / ".orphan_report.txt"
TERMINAL_LIMIT = 30

SYMBOL_RE = re.compile(r"\b((?:fn|sub)_[0-9A-Fa-f]+_0x[0-9a-fA-F]+)\b")
# Broad: matches fn_/sub_ AND fully-renamed/labeled definitions (e.g.
# semaphore_queue_push_0x102d70) -- callers use the generic sub_/fn_ alias
# (see fn_forward_decls.h #define sub_X -> fn_X, or -> a labeled name), so
# matching must key on ADDRESS, not literal symbol text.
DEFINE_RE = re.compile(r"\bvoid\s+([A-Za-z_][A-Za-z0-9_]*_0x[0-9a-fA-F]+)\s*\(")
LABEL_RE = re.compile(r"\blabel_([0-9A-Fa-f]+):")


def addr_of(name: str) -> str:
    # fn_151830_0x151830 -> 151830 (lowercase, matches label_<addr>: convention)
    return name.rsplit("0x", 1)[1].lower()


def parse_args(argv: list[str]) -> tuple[int, int, bool]:
    shard, of = 1, 1
    if "--shard" in argv:
        shard = int(argv[argv.index("--shard") + 1])
    if "--of" in argv:
        of = int(argv[argv.index("--of") + 1])
    if shard < 1 or of < 1 or shard > of:
        print(f"ERROR: invalid --shard {shard} --of {of}", file=sys.stderr)
        sys.exit(1)
    rebuild_cache = "--rebuild-cache" in argv
    return shard, of, rebuild_cache


def scan_defines(paths) -> set[str]:
    defined: set[str] = set()
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="ignore")
        for m in DEFINE_RE.finditer(text):
            defined.add(addr_of(m.group(1)))
    return defined


def build_defined_cache(all_cpp_files: list[Path]) -> set[str]:
    print(f"Building defined-symbol cache from {len(all_cpp_files)} files "
          f"(one-time full pass)...")
    defined = scan_defines(all_cpp_files)
    CACHE_FILE.write_text("\n".join(sorted(defined)), encoding="utf-8")
    print(f"Cached {len(defined)} defined addresses -> {CACHE_FILE.relative_to(ROOT)}\n")
    return defined


def hand_written_defines() -> set[str]:
    """Definitions outside runner/. Deliberately NOT cached: game_overrides.cpp
    and Kernel/recovered/ change far more often than runner/ does, and a stale
    cache here reports a symbol we just added as missing."""
    files = [p for p in (FORWARD_DECLS, OVERRIDES) if p.is_file()]
    if RECOVERED_DIR.is_dir():
        files += sorted(RECOVERED_DIR.glob("*.cpp"))
    return scan_defines(files)


def load_defined_cache(all_cpp_files: list[Path]) -> set[str]:
    if CACHE_FILE.is_file():
        return set(CACHE_FILE.read_text(encoding="utf-8").splitlines())
    return build_defined_cache(all_cpp_files)


def main() -> int:
    if not RUNNER_DIR.is_dir():
        print(f"ERROR: runner dir not found: {RUNNER_DIR}", file=sys.stderr)
        return 1

    shard, of, rebuild_cache = parse_args(sys.argv[1:])

    all_cpp_files = sorted(RUNNER_DIR.glob("*.cpp"))

    if rebuild_cache:
        defined = build_defined_cache(all_cpp_files)
    else:
        defined = load_defined_cache(all_cpp_files)

    defined |= hand_written_defines()

    if of > 1:
        cpp_files = all_cpp_files[shard - 1::of]
        print(f"Shard {shard}/{of}: {len(cpp_files)} of {len(all_cpp_files)} files\n")
    else:
        cpp_files = all_cpp_files

    referenced: set[str] = set()
    labels: dict[str, Path] = {}

    for path in cpp_files:
        text = path.read_text(encoding="utf-8", errors="ignore")
        for m in SYMBOL_RE.finditer(text):
            referenced.add(m.group(1))
        for m in LABEL_RE.finditer(text):
            addr = m.group(1).lower()
            if addr not in labels:
                labels[addr] = path

    orphans = sorted(name for name in referenced if addr_of(name) not in defined)

    if not orphans:
        print(f"OK: {len(referenced)} referenced symbols in this scan, "
              f"{len(defined)} defined addresses globally, 0 orphans")
        return 0

    lines: list[str] = []
    for name in orphans:
        addr = addr_of(name)
        sibling = labels.get(addr)

        if sibling:
            rel = sibling.relative_to(ROOT)
            lines.append(f"  [FOLD]    {name}")
            lines.append(f"            label_{addr}: found in {rel}")
            lines.append(f"            fix: python build_scripts/extract_folded_function.py "
                          f"--file \"{rel}\" --label {addr}")
        else:
            lines.append(f"  [MISSING] {name}")
            lines.append(f"            no label_{addr}: found in this shard's files -- "
                          "if this is a --shard run, re-check with the full unsharded scan "
                          "before treating as genuinely missing")
        lines.append("")

    REPORT_FILE.write_text("\n".join(lines), encoding="utf-8")

    fold_count = sum(1 for l in lines if l.strip().startswith("[FOLD]"))
    missing_count = sum(1 for l in lines if l.strip().startswith("[MISSING]"))
    print(f"Found {len(orphans)} orphaned call target(s): {fold_count} FOLD, {missing_count} MISSING")
    print(f"Full list written to {REPORT_FILE.relative_to(ROOT)}\n")
    print(f"--- first {TERMINAL_LIMIT} lines below ---\n")
    for line in lines[: TERMINAL_LIMIT * 3]:
        print(line)

    return 1


if __name__ == "__main__":
    sys.exit(main())
