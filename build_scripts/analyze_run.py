#!/usr/bin/env python3
"""Query the structured probe sink (run_probe.jsonl) written by ps2x_probe_kv.

WHY THIS EXISTS
---------------
Every diagnostic session so far has ended with a hand-written Select-String
pipeline against a 40 MB console log, and that extraction layer has produced
wrong ANSWERS at least twice -- once because Tee-Object silently switched
between UTF-16 and UTF-8, once because a long record wrapped across two
physical lines and a line-oriented parser glued unrelated records together.
The sink removes the encoding and wrapping problems; this script removes the
"write a new grep each session" problem.

INPUT FORMAT
------------
One JSON object per line. Every value is a hex STRING, without exception,
including counters -- so the reader can do int(v, 16) uniformly and never has
to know which key is an address and which is a count. Automatic fields on
every record: seq, tid, progress, probe.

USAGE
-----
  analyze_run.py                      # summary: record counts per family
  analyze_run.py --derail             # the derail signature (the standing query)
  analyze_run.py --threads            # EE threads + stack-bounds violations
  analyze_run.py --probe RASLOT       # dump one family
  analyze_run.py --probe RASLOT --bad # ... only records with bad != 0
  analyze_run.py --signature          # one-line signature, for the run harness
  analyze_run.py -f path/to.jsonl     # non-default sink location

Exit codes: 0 = query answered, 2 = sink missing/empty/unusable.
"""

import argparse
import json
import os
import sys

DEFAULT_SINK = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "run_probe.jsonl")


def load(path):
    """Return (records, skipped). Records keep hex strings as ints.

    A run is killed by a hard timeout, so the final line can be a partial
    write. That is expected, not an error -- it is counted and reported rather
    than raising, because refusing to analyse an otherwise good 40,000-record
    sink over one truncated tail line would be the wrong trade.
    """
    records, skipped = [], 0
    with open(path, "r", encoding="ascii", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                raw = json.loads(line)
            except ValueError:
                skipped += 1
                continue
            rec = {}
            for k, v in raw.items():
                if k == "probe":
                    rec[k] = v
                else:
                    try:
                        rec[k] = int(v, 16)
                    except (ValueError, TypeError):
                        rec[k] = v
            records.append(rec)
    return records, skipped


def fmt(rec, keys=None):
    """Render one record, hex for everything but the bookkeeping counters."""
    order = keys if keys else [k for k in rec if k != "probe"]
    parts = []
    for k in order:
        if k not in rec:
            continue
        v = rec[k]
        if isinstance(v, int):
            parts.append("%s=%d" % (k, v) if k in ("seq", "n", "progress")
                         else "%s=0x%x" % (k, v))
        else:
            parts.append("%s=%s" % (k, v))
    return "%-10s %s" % (rec.get("probe", "?"), " ".join(parts))


def cmd_summary(records):
    counts = {}
    for r in records:
        counts[r.get("probe", "?")] = counts.get(r.get("probe", "?"), 0) + 1
    print("%d records, %d families" % (len(records), len(counts)))
    for name in sorted(counts, key=lambda n: -counts[n]):
        print("  %-12s %d" % (name, counts[name]))
    if records:
        print("progress span: %d .. %d ticks"
              % (records[0].get("progress", 0), records[-1].get("progress", 0)))
        tids = sorted({r.get("tid", 0) for r in records})
        print("threads seen:  " + " ".join("0x%x" % t for t in tids))


def find_derail(records):
    """First RASLOT with bad!=0, and the first MISS. Both may be absent."""
    bad = next((r for r in records
                if r.get("probe") == "RASLOT" and r.get("bad", 0)), None)
    miss = next((r for r in records if r.get("probe") == "MISS"), None)
    return bad, miss


def cmd_derail(records):
    bad, miss = find_derail(records)
    if bad is None:
        print("no RASLOT record with bad!=0 -- this run did not derail "
              "through the $ra slot.")
    else:
        print("first bad $ra slot:")
        print("  " + fmt(bad, ["seq", "progress", "tid", "n", "slot", "at",
                               "saved", "entryRa", "entrySp", "liveRa"]))
        print("  saved=0x%x vs entryRa=0x%x  (the clobbered value is 'saved')"
              % (bad.get("saved", 0), bad.get("entryRa", 0)))
    if miss is None:
        print("no MISS record -- the dispatcher never lost the guest PC.")
    else:
        print("first dispatch miss:")
        print("  " + fmt(miss, ["seq", "progress", "tid", "pc", "codeRegion"]))
    if bad is not None and miss is not None:
        print("gap: %d probe records, %d progress ticks between clobber and miss"
              % (miss.get("seq", 0) - bad.get("seq", 0),
                 miss.get("progress", 0) - bad.get("progress", 0)))


def cmd_threads(records):
    """EE thread creation, which was completely invisible before Phase C.

    Answers two standing questions in one shot: how many EE threads actually
    exist during boot (a cross-fiber stack stomp is impossible if the answer is
    one), and whether the Thread.cpp threadSp=callerSp fallback ever fires.
    """
    created = [r for r in records if r.get("probe") == "EECREATE"]
    started = [r for r in records if r.get("probe") == "EESTART"]
    oob = [r for r in records if r.get("probe") == "STACKOOB"]

    print("EE threads created: %d, started: %d" % (len(created), len(started)))
    for r in created:
        print("  " + fmt(r, ["seq", "progress", "tid", "entry", "stack",
                             "stackSize", "gp", "prio"]))
    for r in started:
        print("  " + fmt(r, ["seq", "progress", "tid", "entry", "stack",
                             "stackSize", "sp", "callerSp", "borrowed"]))
    borrowed = [r for r in started if r.get("borrowed", 0)]
    if borrowed:
        print("\n!! %d thread(s) started on the CALLER'S stack pointer "
              "(Thread.cpp threadSp=callerSp fallback fired):" % len(borrowed))
        for r in borrowed:
            print("  tid=0x%x entry=0x%x sp=0x%x (borrowed from caller)"
                  % (r.get("tid", 0), r.get("entry", 0), r.get("sp", 0)))
    elif started:
        print("\nno thread borrowed the caller's sp -- the Thread.cpp:398 "
              "fallback did NOT fire in this run.")

    if oob:
        print("\n%d stack-bounds violation(s), first is the one that matters:"
              % len(oob))
        for r in oob:
            print("  " + fmt(r, ["seq", "progress", "pc", "sp", "lo", "hi",
                                 "site", "thid"]))
    else:
        print("\nno STACKOOB records -- no fiber's sp left its registered stack.")


def cmd_signature(records):
    """One line, stable across runs iff the fault is the same fault.

    Deliberately excludes seq/progress/tid: those are timing, and scoring
    timing is what made the previous harness report FAIL on five runs whose
    actual fault was byte-identical.
    """
    bad, miss = find_derail(records)
    print("bad=%s missPc=%s firstBad=[%s]" % (
        "1" if bad else "0",
        "0x%x" % miss["pc"] if miss else "none",
        ("slot=0x%x at=0x%x saved=0x%x entryRa=0x%x entrySp=0x%x"
         % (bad.get("slot", 0), bad.get("at", 0), bad.get("saved", 0),
            bad.get("entryRa", 0), bad.get("entrySp", 0))) if bad else "none"))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-f", "--file", default=DEFAULT_SINK)
    ap.add_argument("--probe", metavar="NAME", help="dump one family")
    ap.add_argument("--bad", action="store_true",
                    help="with --probe, only records whose 'bad' field is set")
    ap.add_argument("--derail", action="store_true")
    ap.add_argument("--threads", action="store_true",
                    help="EE thread creation + stack-bounds violations (Phase C)")
    ap.add_argument("--signature", action="store_true")
    ap.add_argument("--limit", type=int, default=0, help="cap dumped records")
    args = ap.parse_args()

    path = os.path.abspath(args.file)
    if not os.path.exists(path):
        sys.stderr.write("sink not found: %s\n" % path)
        sys.stderr.write("Was PS2X_PROBE_FILE set, and did the run reach a probe?\n")
        return 2

    records, skipped = load(path)
    if not records:
        sys.stderr.write("sink is empty: %s\n" % path)
        return 2
    if skipped:
        sys.stderr.write("note: %d unparseable line(s) skipped "
                         "(a truncated tail line is normal after a timeout kill)\n"
                         % skipped)

    if args.probe:
        rows = [r for r in records if r.get("probe") == args.probe.upper()]
        if args.bad:
            rows = [r for r in rows if r.get("bad", 0)]
        if args.limit:
            rows = rows[:args.limit]
        if not rows:
            print("no %s records matched." % args.probe.upper())
        for r in rows:
            print(fmt(r))
    elif args.derail:
        cmd_derail(records)
    elif args.threads:
        cmd_threads(records)
    elif args.signature:
        cmd_signature(records)
    else:
        cmd_summary(records)
    return 0


if __name__ == "__main__":
    sys.exit(main())
