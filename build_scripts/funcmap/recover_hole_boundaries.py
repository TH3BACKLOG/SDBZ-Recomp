"""Recover the func-map ranges that export_func_map.py silently dropped.

THE BUG THIS FIXES
------------------
export_func_map.py builds the map with:

    for ea in idautils.Functions():
        f = ida_funcs.get_func(ea)
        rows.append((name, f.start_ea, f.end_ea, ...))

In IDA's model a function is a set of CHUNKS. `f.start_ea/f.end_ea` describe
only the ENTRY chunk; every tail chunk is a separate range that this loop never
visits. On MIPS, where the compiler routinely parks epilogues and cold paths
away from the entry (and shares one `jr $ra` tail between several thunks), that
dropped a lot of real code.

Those dropped ranges are exactly the "missing body" dispatch holes. The
recompiler emits a body per CSV row, so a range absent from the CSV gets no
body, and a direct `j` into it dispatches to nothing.

Proof of the diagnosis, from the probe that produced it:

    ea=0x0016e7b8  is_code=True  dword=0x03e00008 (jr $ra)
       get_func = 0x00152f40-0x00152f58  contains=False  flags=0x1000
       add_func = False

`add_func` refuses because the address is already owned; `contains=False`
because the owner's reported range is its entry chunk. Not an undiscovered
function -- an unexported one.

WHAT THIS EMITS
---------------
One CSV row per chunk that is (a) real code IDA knows about and (b) not already
covered by the merged map. Each is a standalone entry point for the recompiler,
which is what dispatch needs: the runtime looks up the jump target, so the body
must START at the target address.

A second pass handles the genuinely-undiscovered leftovers -- addresses IDA
typed as DATA (0x257160 is a real `addiu $sp, $sp, -0x240` prologue sitting in
a data blob). Those need del_items() before add_func() can take.

SAFETY
------
* Database closed with save=False -- the .i64 the GUI uses is NOT modified.
* Rows overlapping an existing map range are WITHHELD and reported, never
  emitted; an overlapping row would re-slice a body that already works.
* Writes exactly one new CSV. Nothing in the repo is overwritten.

⚠ The .i64 must be CLOSED in the IDA GUI -- idalib needs an exclusive lock.

RUN
---
    python build_scripts/find_dispatch_holes.py --class missing-body --addrs-only \
        > build_scripts/funcmap/holes.txt
    python build_scripts/funcmap/recover_hole_boundaries.py --holes build_scripts/funcmap/holes.txt
"""
import argparse
import bisect
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_IDB = os.path.join(ROOT, "ELF", "SLUS_214.42.i64")
DEFAULT_MAP = os.path.join(HERE, "sdbz_func_map_merged.csv")
DEFAULT_OUT = os.path.join(HERE, "recovered_holes.csv")


def load_holes(path):
    addrs, seen = [], set()
    with open(path) as fh:
        for line in fh:
            tok = line.strip().split()[0] if line.strip() else ""
            if tok.lower().startswith("0x") and int(tok, 16) not in seen:
                seen.add(int(tok, 16))
                addrs.append(int(tok, 16))
    return addrs


def load_existing(path):
    ranges = []
    with open(path, newline="") as fh:
        for row in csv.reader(fh):
            try:
                ranges.append((int(row[1], 0), int(row[2], 0), row[0]))
            except (ValueError, IndexError):
                continue  # header and malformed rows
    ranges.sort()
    return ranges


class Cover:
    """Sorted, non-overlapping-lookup view over [start, end) ranges."""

    def __init__(self, ranges):
        self.ranges = sorted(ranges)
        self.starts = [r[0] for r in self.ranges]

    def owner(self, ea):
        i = bisect.bisect_right(self.starts, ea) - 1
        if i >= 0 and self.ranges[i][0] <= ea < self.ranges[i][1]:
            return self.ranges[i]
        return None

    def overlap(self, lo, hi):
        i = bisect.bisect_right(self.starts, lo) - 1
        for j in (i, i + 1):
            if 0 <= j < len(self.ranges):
                s, e, name = self.ranges[j]
                if s < hi and lo < e:
                    return self.ranges[j]
        return None

    def add(self, lo, hi, name):
        bisect.insort(self.ranges, (lo, hi, name))
        self.starts = [r[0] for r in self.ranges]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--holes", required=True, help="file of 0x addresses, one per line")
    ap.add_argument("--idb", default=DEFAULT_IDB)
    ap.add_argument("--funcmap", default=DEFAULT_MAP)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--all-chunks", action="store_true",
                    help="emit EVERY unexported chunk, not just those needed by a hole. "
                         "Closes the class outright; produces many more rows.")
    args = ap.parse_args()

    holes = load_holes(args.holes)
    if not holes:
        sys.exit(f"no addresses parsed from {args.holes}")
    cover = Cover(load_existing(args.funcmap))
    print(f"{len(holes)} holes; {len(cover.ranges)} existing map rows")

    import idapro  # must be imported before any other ida_* module

    print(f"opening {args.idb} ...")
    if idapro.open_database(args.idb, run_auto_analysis=True) != 0:
        sys.exit("FAILED to open database (is it open in the IDA GUI?)")

    import ida_auto
    import ida_bytes
    import ida_funcs
    import ida_name
    import idautils

    hole_set = set(holes)
    rows = []
    notes = []
    tally = {"chunk": 0, "created": 0, "overlap": 0, "not-at-start": 0,
             "unresolved": 0, "degenerate": 0}

    def emit(lo, hi, name, why):
        if hi <= lo:
            tally["degenerate"] += 1
            notes.append(f"0x{lo:08x}  DEGENERATE end 0x{hi:08x} <= start")
            return False
        hit = cover.overlap(lo, hi)
        if hit:
            tally["overlap"] += 1
            notes.append(f"0x{lo:08x}  OVERLAP    0x{lo:08x}-0x{hi:08x} collides with "
                         f"{hit[2]} 0x{hit[0]:08x}-0x{hit[1]:08x}  (WITHHELD)")
            return False
        if "," in name:
            name = name.replace(",", "_")  # a comma silently kills the row
        rows.append((name, f"0x{lo:08x}", f"0x{hi:08x}", f"0x{hi - lo:x}"))
        cover.add(lo, hi, name)
        tally[why] += 1
        return True

    try:
        ida_auto.auto_wait()

        # ---- Pass 1: every chunk IDA knows about that the map never exported.
        # Sorted so that a chunk covering several holes is emitted once, and so
        # emission order is deterministic between runs.
        chunks = {}
        for fea in idautils.Functions():
            fname = ida_name.get_ea_name(fea, ida_name.GN_VISIBLE) or f"sub_{fea:08X}"
            for lo, hi in idautils.Chunks(fea):
                if lo == fea:
                    continue  # entry chunk -- already exported
                chunks.setdefault((lo, hi), fname)

        print(f"IDA knows {len(chunks)} tail chunks")
        for (lo, hi), parent in sorted(chunks.items()):
            if cover.owner(lo):
                continue  # already mapped
            wanted = args.all_chunks or any(lo <= h < hi for h in hole_set)
            if not wanted:
                continue
            emit(lo, hi, f"{parent}_tail{lo:08X}", "chunk")

        # ---- Pass 2: holes still uncovered. These are addresses IDA never
        # made into code at all -- typically a prologue buried in a data blob.
        for ea in holes:
            if cover.owner(ea):
                continue
            f = ida_funcs.get_func(ea)
            if f is None:
                ida_bytes.del_items(ea, ida_bytes.DELIT_EXPAND)
                ida_auto.auto_wait()
                ida_funcs.add_func(ea)
                ida_auto.auto_wait()
                f = ida_funcs.get_func(ea)
            if f is not None and f.start_ea == ea:
                nm = ida_name.get_ea_name(ea, ida_name.GN_VISIBLE) or f"sub_{ea:08X}"
                emit(f.start_ea, f.end_ea, nm, "created")
                continue
            tally["unresolved"] += 1
            owner = f"owned by 0x{f.start_ea:08x}" if f is not None else "IDA refused"
            notes.append(f"0x{ea:08x}  UNRESOLVED {owner}")
    finally:
        print("closing database (save=False -- .i64 untouched) ...")
        idapro.close_database(save=False)

    rows = sorted(set(rows), key=lambda r: int(r[1], 16))
    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["name", "start", "end", "size"])
        w.writerows(rows)

    # How many of the original holes does the recovered map actually cover, and
    # how many land exactly on a new row's START (which is what dispatch needs)?
    new = Cover([(int(r[1], 16), int(r[2], 16), r[0]) for r in rows])
    new_starts = {int(r[1], 16) for r in rows}
    covered = sum(1 for h in holes if new.owner(h))
    at_start = sum(1 for h in holes if h in new_starts)

    print()
    for n in notes[:80]:
        print("  " + n)
    if len(notes) > 80:
        print(f"  ... {len(notes) - 80} more")
    print()
    print(f"wrote {args.out}: {len(rows)} rows")
    for k in sorted(tally):
        print(f"  {k:<13}: {tally[k]}")
    print()
    print(f"holes covered by a new row : {covered}/{len(holes)}")
    print(f"  ...landing on its START  : {at_start}   <- these dispatch correctly")
    print(f"  ...landing mid-row       : {covered - at_start}   <- need a split, see below")
    print()
    print("NEXT: patch_func_map.py, then assert the merged row count grew by exactly "
          f"{len(rows)} (malformed rows are skipped SILENTLY, exit 0).")


if __name__ == "__main__":
    main()
