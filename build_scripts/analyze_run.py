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
  analyze_run.py --coverage           # what the EE actually dispatched (PS2_COVERAGE=1)
  analyze_run.py --coverage --band game   # ... game band only (default: both)
  analyze_run.py --coverage --diff b.jsonl  # addresses in one run and not the other
  analyze_run.py --onset              # when each watched field froze (causality order)
  analyze_run.py --watch g36          # time series for one watched field
  analyze_run.py --probe TRACE        # [trace] intercepted guest calls
  analyze_run.py --hwwatch            # watchpoint hits + their call sites
  analyze_run.py --hwwatch --val 8a6440   # ... only hits storing that value
  analyze_run.py --hwwatch --all-runs # ... every run in the dump, not the last
  analyze_run.py --runs               # every archived run: how far the movie path got
  analyze_run.py --compare A.txt B.txt  # cross-run tag diff + cap symmetry check
  analyze_run.py --tag movie --log X.txt # [movie] records from a specific log
  analyze_run.py --gs                 # GS render path (reads run_log.txt)
  analyze_run.py --gs --log other.txt # ... from a non-default log
  analyze_run.py -f path/to.jsonl     # non-default sink location

Exit codes: 0 = query answered, 2 = sink missing/empty/unusable.
"""

import argparse
import collections
import json
import os
import re
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


# ---- coverage (PS2_COVERAGE=1) ---------------------------------------------
#
# Record shapes emitted by scanCoverage() in ps2_runtime.cpp:
#   COVERAGE phase distinct game sdk calls slots   -- one per dump, periodic
#   COVTOP   rank addr count game                  -- top 48, one per dump
#   COVGAME  addr count                            -- EVERY game-band address,
#                                                     shutdown dump only
#
# ASYMMETRY THAT MATTERS: COVGAME is the only exhaustive set, and it is
# game-band only. The SDK band is visible ONLY through COVTOP's 48 rows, so
# `--band sdk` is a top-48 view, never a census. Said out loud in the output
# rather than left for the reader to rediscover.

COVERAGE_CAVEAT = (
    "NOTE: counts are TABLE-DISPATCHED calls only. A direct fn_* C++ call\n"
    "      bypasses lookupFunction. Nonzero proves 'this ran'; ZERO proves\n"
    "      only 'never dispatched', NOT 'never executed'. Do not argue\n"
    "      absence from this data alone (ps2_runtime.cpp:404-408)."
)


def coverage_sets(records):
    """Return (summaries, top, game) for one run's records.

    summaries: COVERAGE rows in emission order (last is the shutdown dump).
    top:       {addr: (count, is_game)} from the LAST COVTOP block.
    game:      {addr: count} from COVGAME (exhaustive, game band only).
    """
    summaries = [r for r in records if r.get("probe") == "COVERAGE"]

    # COVTOP is re-emitted every dump; keep only the final block, identified by
    # rank restarting at 0. Mixing blocks would double-count the same address.
    top = {}
    for r in records:
        if r.get("probe") != "COVTOP":
            continue
        if r.get("rank", 0) == 0:
            top = {}
        top[r.get("addr", 0)] = (r.get("count", 0), bool(r.get("game", 0)))

    game = {}
    for r in records:
        if r.get("probe") == "COVGAME":
            game[r.get("addr", 0)] = r.get("count", 0)
    return summaries, top, game


def cmd_coverage(records, band, limit, diff_path):
    summaries, top, game = coverage_sets(records)

    if not summaries and not top and not game:
        print("no coverage records -- PS2_COVERAGE was not set for this run.")
        print("Arm it with:  $env:PS2_COVERAGE = \"1\"")
        return

    if summaries:
        print("coverage dumps: %d" % len(summaries))
        for r in summaries:
            print("  " + fmt(r, ["phase", "distinct", "game", "sdk", "calls",
                                 "slots"]))
        f = summaries[-1]
        dist = f.get("distinct", 0)
        if dist:
            print("\nfinal: %d distinct addresses dispatched "
                  "(%d game / %d sdk), %d total calls"
                  % (dist, f.get("game", 0), f.get("sdk", 0), f.get("calls", 0)))
            print("       %.1f%% of the %d-slot code span was ever entered"
                  % (100.0 * dist / max(f.get("slots", 1), 1), f.get("slots", 0)))

    if game:
        print("\nCOVGAME census: %d distinct game-band addresses, %d calls"
              % (len(game), sum(game.values())))
    elif summaries:
        print("\nno COVGAME rows -- the shutdown full dump did not run "
              "(run killed before clean exit?).")

    if diff_path is not None:
        cmd_coverage_diff(records, diff_path, band)
        return

    rows = []
    if band in ("game", None) and game:
        rows += [(a, c, "game") for a, c in game.items()]
    if band in ("sdk", None):
        rows += [(a, c, "sdk") for a, (c, isg) in top.items() if not isg]
        if band == "sdk":
            print("\n(SDK band is COVTOP-only -- top 48, not a census.)")
    if band == "game" and not game and top:
        rows += [(a, c, "game") for a, (c, isg) in top.items() if isg]

    rows.sort(key=lambda t: -t[1])
    n = limit if limit else 40
    print("\ntop %d by dispatch count:" % min(n, len(rows)))
    for addr, count, b in rows[:n]:
        print("  0x%08x  %10d  %s" % (addr, count, b))
    if len(rows) > n:
        print("  ... %d more (raise with --limit)" % (len(rows) - n))

    print("\n" + COVERAGE_CAVEAT)


def cmd_coverage_diff(records, other_path, band):
    """Addresses present in one run and not the other.

    This is the query that makes run-over-run comparison possible at all: after
    a fix, the useful question is never 'what ran' but 'what runs NOW that did
    not before' -- and the inverse, which catches a fix that silently removed
    execution.
    """
    other_path = os.path.abspath(other_path)
    if not os.path.exists(other_path):
        sys.stderr.write("diff sink not found: %s\n" % other_path)
        return
    other, _ = load(other_path)
    _, btop, bgame = coverage_sets(other)
    _, atop, agame = coverage_sets(records)

    # Union the exhaustive game census with COVTOP so an SDK-band address that
    # appears in one run's top 48 and not the other's is still reported -- with
    # its band labelled, so nobody reads a top-48 artefact as a census result.
    def merged(gamemap, topmap):
        out = {}
        for a, c in gamemap.items():
            out[a] = (c, "game")
        for a, (c, isg) in topmap.items():
            if a not in out:
                out[a] = (c, "game" if isg else "sdk")
        return out

    a = merged(agame, atop)
    b = merged(bgame, btop)
    if band:
        a = {k: v for k, v in a.items() if v[1] == band}
        b = {k: v for k, v in b.items() if v[1] == band}

    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    both = sorted(set(a) & set(b))

    print("\ndiff vs %s" % other_path)
    print("  this run: %d addrs   other: %d addrs   shared: %d"
          % (len(a), len(b), len(both)))

    print("\n  ONLY in this run (%d):" % len(only_a))
    for addr in sorted(only_a, key=lambda x: -a[x][0]):
        print("    +0x%08x  %10d  %s" % (addr, a[addr][0], a[addr][1]))

    print("\n  ONLY in %s (%d):" % (os.path.basename(other_path), len(only_b)))
    for addr in sorted(only_b, key=lambda x: -b[x][0]):
        print("    -0x%08x  %10d  %s" % (addr, b[addr][0], b[addr][1]))

    if not only_a and not only_b:
        print("\n  identical executed sets. If a fix was expected to change "
              "control flow, it did not.")

    print("\n" + COVERAGE_CAVEAT)


# ---- GS probes (--gs) -------------------------------------------------------
#
# ASYMMETRY WITH EVERYTHING ABOVE: the GS probes predate the JSONL sink and
# still go out through RUNTIME_LOG, so they land in run_log.txt and NOT in
# run_probe.jsonl. That file is UTF-16, which has burned this project twice --
# `Select-String -Encoding unicode` returns garbage on it, and the hand-written
# regex has been rewritten from scratch every session. This command is that
# regex, written once.
#
# Every [fbdest]/[blackwho] counter is read with exchange(0) at report time, so
# every printed line is a PER-INTERVAL DELTA, not a running total. Two identical
# consecutive lines mean a steady-state workload, not a stuck probe.

GS_TAGS = ("gs:frame", "fbdest", "blackwho", "fbscan", "vramcensus",
           "gsdump", "present")

DEFAULT_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "run_log.txt")


def read_text_any(path):
    """Decode a log whatever its encoding. UTF-16 is the usual case here."""
    with open(path, "rb") as fh:
        raw = fh.read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16", errors="replace")
    for enc in ("utf-8", "utf-16-le", "latin-1"):
        try:
            text = raw.decode(enc)
        except (UnicodeDecodeError, ValueError):
            continue
        # A UTF-16 file read as utf-8 survives only if it is mostly NULs.
        if text.count("\x00") > len(text) // 8:
            continue
        return text
    return raw.decode("latin-1", errors="replace")


def cmd_tag(log_path, tag, limit):
    """Dump every [tag] record from the console log, plus its cap verdict.

    run_log.txt is UTF-16 and is NOT reliably newline-delimited, so a
    Select-String / readline approach returns the whole file as one hit.
    Split on the tag itself instead.

    The cap line matters as much as the records: a saturated probe is
    indistinguishable from one that never fired, so absence of a [cap] for
    this tag is what makes a short result trustworthy.
    """
    if not os.path.exists(log_path):
        sys.stderr.write("log not found: %s\n" % log_path)
        return 2
    text = read_text_any(log_path)
    name = tag.strip("[]")
    # Match the bare tag OR a colon-namespaced sub-tag (e.g. "semwatch" also
    # catches "[semwatch:wait]", "[semwatch:signal]", "[semwatch:block]").
    # Without this, --tag semwatch silently returned 0 records against a log
    # that actually had 10 -- an exact-bracket-only match on a family of
    # sub-tagged probes reads as "never fired" instead of "wrong query", which
    # is exactly the false-negative-by-omission trap this tool exists to
    # prevent (2026-08-29). A caller who already passes the full sub-tag
    # (e.g. "semwatch:wait") is unaffected -- the optional group just doesn't
    # trigger.
    recs = re.findall(r"\[" + re.escape(name) + r"(?::[^\[\]]*)?\][^\[]*", text)
    caps = [c.strip() for c in re.findall(r"\[cap\][^\[]*", text)
            if name in c]

    shown = recs[:limit] if limit else recs
    for r in shown:
        print(r.strip())
    print("-- [%s] records=%d shown=%d" % (name, len(recs), len(shown)))
    if caps:
        print("-- CAPPED -- absence of a value past this point is NOT evidence:")
        for c in caps:
            print("   " + c)
    else:
        print("-- no [cap] for this tag: absence IS evidence.")
    return 0


DEFAULT_ARCHIVE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "logs", "archive")

# Markers that decide whether a run got anywhere near the Stage 5.15 movie open.
# Kept as plain substrings / cheap regexes on purpose: run_log.txt has records
# that are tens of KB long, so any pattern of the form [^\n]*X[^\n]* backtracks
# catastrophically and hangs. Never reintroduce one here.
#
# 2026-08-17 -- the "sfd" column used to be re.compile(r"DVCI|\.SFD|SFD;1").
# That is three alternations over ONE guest printf,
#
#     DVCI: File cache was not hit. "\MOVIE\ATARI.SFD;1"
#
# which matches "DVCI" and then ".SFD" and so scores 2 for a single event.
# Every "sfd=2" ever printed by this tool meant "one DVCI open", and reading
# it as two opens is exactly the kind of magnitude error that sent this
# investigation after an intermittency that was never there. The column now
# counts the printf itself, once, and is named after what it measures.
RUN_MARKERS = [
    ("dvci", re.compile(r"DVCI:")),
    ("597", re.compile(r"80000597")),
    # The Stage 5.15 chain, in the order it has to happen. Each is one record
    # per event, so these columns are counts of events and not of substrings.
    ("mvreq", re.compile(r"fn=124dc8")),
    ("mvopen", re.compile(r"fn=130ef0")),
    ("cdsrch", re.compile(r"fn=1866a0")),
    ("f80", re.compile(r"f80=0x(?!0+\b)[0-9a-f]+")),
    # Dispatch holes. "holes" is the per-distinct-target census added in run 62;
    # the older [guest-branch:missing-target] dump is ONE global one-shot, so a
    # run can show missing=1 while having hit a dozen holes. Read "holes".
    ("holes", re.compile(r"\[hole\] n=")),
    # The libcdvd SIF-RPC end_function at 0x186310, the run-61 blocker. Nonzero
    # here means the completion callback that unblocks async libcdvd actually ran.
    ("rpcend", re.compile(r"\[rpcend\] fn=186310")),
    # cdvdfsv N-commands (sid 0x80000595), added run 63. "ncmdrd" counts only
    # the reads that actually moved bytes -- a served-but-failed read and an
    # unserved one are different failures and must not share a column.
    # NOTE: from run 64 the emitter decimates repeats, so "ncmd" is a count of
    # EMITTED records, not of N-commands issued. Read "ncmdnew" for the number
    # of distinct (fno,lbn,sectors) targets -- that is the number that answers
    # "did the guest ever move on to another part of the file".
    ("ncmd", re.compile(r"\[iop:ncmd\] ")),
    ("ncmdnew", re.compile(r"\[iop:ncmd\][^\r\n]{0,40}?NEW fno=")),
    ("ncmdrd", re.compile(r"\[iop:ncmd\].{0,80}?fno=1 handled=1.{0,90}?readOk=1")),
    # Did real .SFD bytes land in the guest buffer? A .SFD is an MPEG program
    # stream, so sector 0 begins 00 00 01 BA; dstHead is that little-endian u64,
    # which puts BA010000 in the low half. readOk=1 only means the reader
    # returned true -- this column is the one that proves bytes arrived.
    ("sfdhd", re.compile(r"dstHead=0x[0-9A-Fa-f]{8}[Bb][Aa]010000")),
    ("rpcmiss", re.compile(r"sif:rpc-miss\]")),
    ("caps", re.compile(r"\[cap\] tag=")),
]

# A run this small never got past boot. They are still listed -- hiding a run
# is how "it never happens" gets manufactured -- but they are flagged, because
# an unflagged 400 KB fragment sitting in the table reads as a full run that
# produced no evidence.
FRAGMENT_BYTES = 1 << 20

ADX_RE = re.compile(r"\[adx:stream\] t=(\d+)s nonzero=(\d+)/\d+ maxst=(\d+)")
TAG_RE = re.compile(r"\[([A-Za-z][A-Za-z0-9_:.\-]{1,28})\]")
CAP_TAG_RE = re.compile(r"\[cap\] tag=([A-Za-z0-9_:.\-]+)")


def _run_logs(archive_dir, extra=None):
    """Archived run logs, oldest first, with the live run_log.txt appended.

    launch_recomp.ps1 archives the live log by COPYING it, so immediately
    after a batch the live run_log.txt is byte-identical to the newest
    archived one. Appending it unconditionally listed that run twice and
    inflated every "N/M runs reached ..." denominator by one. The live log is
    therefore skipped when an archived log already has its exact size.
    """
    out = []
    sizes = set()
    if os.path.isdir(archive_dir):
        for name in sorted(os.listdir(archive_dir)):
            if name.startswith("run_log.") and name.endswith(".txt"):
                p = os.path.join(archive_dir, name)
                out.append(p)
                try:
                    sizes.add(os.path.getsize(p))
                except OSError:
                    pass
    live = os.path.abspath(extra or DEFAULT_LOG)
    if os.path.exists(live) and os.path.getsize(live) not in sizes:
        out.append(live)
    return out


def _parked_count(archive_dir):
    """Runs moved to archive/old/. Reported so the inventory can never imply
    that the runs it lists are all the runs there are."""
    old = os.path.join(archive_dir, "old")
    if not os.path.isdir(old):
        return 0
    return len([n for n in os.listdir(old)
                if n.startswith("run_log.") and n.endswith(".txt")])


def cmd_runs(archive_dir, live_log):
    """One row per archived run: how far the movie path actually got.

    The point of this command is to make INTERMITTENCY visible. Reading one run
    at a time made a 2-in-7 event look like "it never happens", which is what
    sent Stage 5.15 chasing sid 0x80000597 for three run cycles.
    """
    logs = _run_logs(archive_dir, live_log)
    if not logs:
        sys.stderr.write("no run logs found under %s\n" % archive_dir)
        return 2

    hdr = ("%-22s %8s %6s %5s %6s %5s" %
           ("run", "bytes", "adxrec", "st1@", "maxst", "max@"))
    hdr += "".join(" %7s" % n for n, _ in RUN_MARKERS)
    print(hdr)
    print("-" * len(hdr))

    reached = []
    fragments = []
    for path in logs:
        text = read_text_any(path)
        recs = ADX_RE.findall(text)
        if recs:
            st1 = next((int(t) for t, _n, s in recs if int(s) >= 1), None)
            mx = max(int(s) for _t, _n, s in recs)
            mxat = next((int(t) for t, _n, s in recs if int(s) == mx), None)
            adx = (len(recs), st1, mx, mxat)
        else:
            adx = (0, None, None, None)

        counts = [len(rx.findall(text)) for _n, rx in RUN_MARKERS]
        size = os.path.getsize(path)
        name = os.path.basename(path)
        name = name[len("run_log."):-len(".txt")] if name.startswith("run_log.") \
            and name != "run_log.txt" else name
        if size < FRAGMENT_BYTES:
            fragments.append(name)
            name = name + "*"
        row = ("%-22s %8d %6d %5s %6s %5s" %
               (name[:22], size, adx[0],
                "-" if adx[1] is None else adx[1],
                "-" if adx[2] is None else adx[2],
                "-" if adx[3] is None else adx[3]))
        row += "".join(" %7d" % c for c in counts)
        print(row)
        # Reaching the open is what the "dvci" column measures directly: the
        # guest only prints that line from inside sub_130EF0. The old test
        # also required the 597 column, which is a DIFFERENT event (the
        # libcdvd RPC bind) that does not fire on every open -- so runs that
        # demonstrably opened the file were being counted as not reaching it.
        if counts[0]:
            reached.append(name)

    print()
    print("runs that reached the .SFD open: %d/%d  %s" %
          (len(reached), len(logs), " ".join(reached) or "(none)"))
    if fragments:
        print("* fragment (<%d KB): ended before boot completed; its zero"
              % (FRAGMENT_BYTES // 1024))
        print("  columns mean 'run did not get that far', NOT 'event absent'.")
        print("  %s" % " ".join(fragments))
    print("NOTE: a run with adxrec=0 predates the [adx:stream] probe -- its")
    print("      blank columns mean 'not instrumented', NOT 'did not happen'.")
    print("NOTE: dvci/mvreq/mvopen/cdsrch are EVENT counts (one per record).")
    print("      A zero in cdsrch with mvopen>0 means the DVCI open returned")
    print("      without ever attempting the disc lookup.")
    parked = _parked_count(archive_dir)
    if parked:
        print("NOTE: %d further run(s) are parked in archive/old/ and are NOT"
              % parked)
        print("      counted above. See archive/old/README.md.")
    return 0


def cmd_compare(path_a, path_b, limit):
    """Cross-run tag diff: counts side by side, plus cap status on BOTH sides.

    Cap status is not decoration. A tag capped in A and uncapped in B produces a
    count difference that says nothing about the guest, and this project has
    already burned runs on exactly that kind of false diff.
    """
    for p in (path_a, path_b):
        if not os.path.exists(p):
            sys.stderr.write("log not found: %s\n" % p)
            return 2

    ta, tb = read_text_any(path_a), read_text_any(path_b)
    ca = collections.Counter(TAG_RE.findall(ta))
    cb = collections.Counter(TAG_RE.findall(tb))
    capa = set(CAP_TAG_RE.findall(ta))
    capb = set(CAP_TAG_RE.findall(tb))

    print("A = %s" % path_a)
    print("B = %s" % path_b)
    print()

    def capmark(tag, caps):
        return "CAP" if any(tag == c or tag.startswith(c) for c in caps) else "."

    rows = []
    for tag in set(ca) | set(cb):
        a, b = ca[tag], cb[tag]
        if a == b:
            continue
        oneside = (a == 0) != (b == 0)
        ratio = (max(a, b) / max(1, min(a, b)))
        if oneside or (ratio >= 3 and max(a, b) >= 5):
            rows.append((oneside, max(a, b), tag, a, b))
    rows.sort(key=lambda r: (not r[0], -r[1]))

    print("%-30s %8s %4s %8s %4s  %s" % ("tag", "A", "capA", "B", "capB", "kind"))
    print("-" * 72)
    shown = rows[:limit] if limit else rows
    for oneside, _mx, tag, a, b in shown:
        print("%-30s %8d %4s %8d %4s  %s" %
              (tag[:30], a, capmark(tag, capa), b, capmark(tag, capb),
               "ONE-SIDED" if oneside else "ratio"))
    if not shown:
        print("(no tag differs by one-sided presence or a >=3x ratio)")
    print()
    if rows and len(shown) < len(rows):
        print("... %d more rows suppressed by --limit" % (len(rows) - len(shown)))

    onlya = sorted(capa - capb)
    onlyb = sorted(capb - capa)
    if onlya or onlyb:
        print("ASYMMETRIC CAPS -- differences for these tags are NOT evidence:")
        if onlya:
            print("   capped in A only: %s" % " ".join(onlya))
        if onlyb:
            print("   capped in B only: %s" % " ".join(onlyb))
    else:
        print("caps are symmetric: count differences above are real.")
    return 0


def parse_gs(text):
    """{tag: [ {k: v}, ... ]} in emission order.

    Splits on the tag rather than on newlines: a long record that wrapped
    across two physical lines is the exact failure that produced a wrong answer
    in an earlier session, and a line-oriented parser cannot see it.
    """
    kv_re = re.compile(r"([A-Za-z_][\w.:]*)=(\S+)")
    out = {}
    for tag in GS_TAGS:
        body_re = re.compile(r"\[" + re.escape(tag) + r"\]([^\[]*)")
        rows = []
        for m in body_re.finditer(text):
            rows.append(dict(kv_re.findall(m.group(1))))
        if rows:
            out[tag] = rows
    return out


def _num(rec, key, default=0):
    v = rec.get(key)
    if v is None:
        return default
    try:
        return int(v, 16) if v.lower().startswith("0x") else int(v)
    except ValueError:
        return default


def cmd_gs(log_path, limit):
    if not os.path.exists(log_path):
        sys.stderr.write("log not found: %s\n" % log_path)
        return 2

    text = read_text_any(log_path)   # 40 MB; read once, never per query
    tags = parse_gs(text)
    if not tags:
        print("no GS probe records in %s" % log_path)
        print("Expected tags: " + ", ".join("[%s]" % t for t in GS_TAGS))
        return 0

    print("GS probes in %s" % os.path.abspath(log_path))
    for tag in GS_TAGS:
        rows = tags.get(tag)
        if not rows:
            continue
        # [fbscan]/[gsdump] emit several rows per report interval; show a whole
        # trailing group rather than one row, or the buffers get compared
        # across different intervals.
        if limit:
            n = limit
        elif tag == "vramcensus":
            n = 8          # one full top-N page ranking
        elif tag in ("fbscan", "gsdump"):
            n = 4          # one full candidate-fbp sweep
        else:
            n = 2
        print("\n[%s]  %d record(s), last %d:" % (tag, len(rows), min(n, len(rows))))
        for r in rows[-n:]:
            print("  " + " ".join("%s=%s" % kv for kv in r.items()))

    # ---- the two readings that are always wanted, spelled out ----
    bw = tags.get("blackwho")
    if bw:
        r = bw[-1]
        untex = _num(r, "untex")
        texzero = _num(r, "texzero")
        texcol = _num(r, "texcol")
        total = untex + texzero + texcol
        if total:
            print("\nblack-pixel attribution (last interval, %d px):" % total)
            for name, v in (("untex", untex), ("texzero", texzero),
                            ("texcol", texcol)):
                print("  %-8s %12d  %5.1f%%" % (name, v, 100.0 * v / total))
            if texcol * 20 < total:
                print("  -> texcol is noise: blend / TEXFUNC / FBMSK are NOT "
                      "the cause. Colour is already black entering writePixel.")
            if texzero > untex:
                print("  -> texzero dominates: textured sprites are sampling to "
                      "RGB 0. Suspect the PSMT4 unswizzle or the CLUT.")

    fs = tags.get("fbscan")
    if fs:
        tail = fs[-4:]
        live = [r for r in tail if _num(r, "nonblack") > 0]
        print("\nfbscan (last group): %d of %d candidate buffers non-black"
              % (len(live), len(tail)))
        if not live:
            print("  -> every candidate fbp decodes to pure black. A correct "
                  "double-buffered clear cannot blacken BOTH buffers, so this "
                  "is not just a swap/flip bug.")

    caps = re.findall(r"\[cap\] tag=(\S+)", text)
    if caps:
        print("\n!! %d saturated probe(s) in this run: %s"
              % (len(caps), " ".join(sorted(set(caps)))))
        print("   A capped tag's ABSENCE proves nothing. GS tags are unaffected "
              "unless they appear in that list.")
    return 0


DEFAULT_HWDUMP_SUFFIX = ".hwwatch.txt"

HW_HIT_RE = re.compile(
    r"^=== HWWATCH hit #(\d+)\s+guest=0x([0-9a-fA-F]+)\s+"
    r"newval=0x([0-9a-fA-F]+)\s+tid=0x([0-9a-fA-F]+) ===")
HW_FRAME_RE = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+(.+?)(?:\s{2,}\[(.+)\])?$")

# Frames that are always present because the VEH itself is on the stack. They
# are noise in every single hit, so the report drops them by default.
HW_VEH_NOISE = ("hwWatchVeh", "RtlLocateExtendedFeature",
                "KiUserExceptionDispatcher", "RtlRaiseException")


def parse_hwwatch_dump(path):
    """Return [run, ...]; each run is a list of hit dicts, oldest run first.

    The dump is opened "a" by the runtime, so it ACCUMULATES across every run
    ever done. Hit indices restart at 0 each run, so a non-increasing index is
    the run boundary -- there is no other marker in the file.
    """
    runs, cur, hit = [], [], None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = HW_HIT_RE.match(line)
            if m:
                n = int(m.group(1))
                if hit is not None:
                    cur.append(hit)
                if cur and n <= cur[-1]["n"]:
                    runs.append(cur)
                    cur = []
                hit = {"n": n, "guest": int(m.group(2), 16),
                       "val": int(m.group(3), 16), "tid": int(m.group(4), 16),
                       "site": None, "stack": []}
                continue
            if hit is None:
                continue
            f = HW_FRAME_RE.match(line)
            if not f:
                continue
            frame = {"rip": int(f.group(1), 16), "sym": f.group(2).strip(),
                     "src": (f.group(3) or "").strip()}
            if hit["site"] is None:
                hit["site"] = frame
            else:
                hit["stack"].append(frame)
    if hit is not None:
        cur.append(hit)
    if cur:
        runs.append(cur)
    return runs


def _hw_short(frame):
    """'sym + off  [file:line]' with the repo prefix stripped off the path."""
    if frame is None:
        return "<unsymbolized>"
    src = frame["src"]
    if src:
        src = re.sub(r"^.*[\\/](?:runner|lib)[\\/]", "", src)
        return "%s   [%s]" % (frame["sym"], src)
    return frame["sym"]


def cmd_hwwatch(sink_path, dump_path, value_filter, all_runs, limit):
    """Join HWWATCH stacks to their values: 'who stores WHAT, and from where'.

    The numeric sink and the symbolized dump answer half the question each --
    the sink has the values and the cap accounting, the dump has the call
    sites. Joining them by hand cost ~15 tool calls in the session that first
    needed it; that is what this exists to remove.
    """
    if not os.path.exists(dump_path):
        sys.stderr.write("hwwatch dump not found: %s\n" % dump_path)
        sys.stderr.write("Was the run launched with -HwWatch and "
                         "PS2X_HWWATCH_ADDR set?\n")
        return 2

    runs = parse_hwwatch_dump(dump_path)
    if not runs:
        sys.stderr.write("no HWWATCH hits in %s\n" % dump_path)
        return 2

    hits = [h for r in runs for h in r] if all_runs else runs[-1]
    print("%s" % os.path.abspath(dump_path))
    print("%d run(s) in dump; reporting %s -> %d hit(s)"
          % (len(runs), "ALL runs" if all_runs else "the LAST run only",
             len(hits)))

    # ---- cap accounting, from the numeric sink ----
    if os.path.exists(sink_path):
        records, _ = load(sink_path)
        emitted = len([r for r in records if r.get("probe") == "HWWATCH"])
        stats = [r for r in records if r.get("probe") == "HWSTAT"]
        counted = stats[-1].get("hits", 0) if stats else 0
        print("sink: %d HWWATCH record(s), HWSTAT counted %d hit(s)"
              % (emitted, counted))
        if counted > emitted:
            print("  !! CAPPED: %d hit(s) discarded. The dump holds the "
                  "EARLIEST hits only -- absence of a value here proves "
                  "NOTHING." % (counted - emitted))

    if value_filter is not None:
        hits = [h for h in hits if h["val"] == value_filter]
        print("filtered to val=0x%x -> %d hit(s)" % (value_filter, len(hits)))
        if not hits:
            return 0

    # ---- value x call-site histogram ----
    combos = {}
    for h in hits:
        key = (h["val"], _hw_short(h["site"]))
        combos.setdefault(key, []).append(h)

    guests = sorted({h["guest"] for h in hits})
    print("\nwatched address(es): " + " ".join("0x%x" % g for g in guests))
    print("\nvalue -> store site:")
    for (val, site), rows in sorted(combos.items(),
                                    key=lambda kv: -len(kv[1])):
        tids = sorted({h["tid"] for h in rows})
        print("  0x%-10x %4d hit(s)  tid=%s"
              % (val, len(rows), ",".join("0x%x" % t for t in tids)))
        print("      %s" % site)

    # ---- one exemplar stack per distinct value ----
    shown = set()
    for (val, _site), rows in sorted(combos.items(),
                                     key=lambda kv: -len(kv[1])):
        if val in shown:
            continue
        shown.add(val)
        if limit and len(shown) > limit:
            break
        h = rows[-1]
        print("\ncaller chain for val=0x%x (hit #%d, tid=0x%x):"
              % (val, h["n"], h["tid"]))
        depth = 0
        for f in h["stack"]:
            if any(f["sym"].startswith(p) or p in f["sym"]
                   for p in HW_VEH_NOISE):
                continue
            print("  %2d  %s" % (depth, _hw_short(f)))
            depth += 1
    return 0


WATCH_META = ("seq", "tid", "progress", "probe", "t", "unreadable")


def _watch_rows(records):
    """WATCH samples in time order, plus the field names they carry.

    Field order follows first appearance rather than sorted(), so the table
    reads in the order the operator wrote PS2X_TRACE_WATCH.
    """
    rows = [r for r in records if r.get("probe") == "WATCH"]
    rows.sort(key=lambda r: r.get("t", 0))
    fields = []
    for r in rows:
        for k in r:
            if k not in WATCH_META and k not in fields:
                fields.append(k)
    return rows, fields


def _field_track(rows, field):
    """(first_t, last_change_t, changes, first_val, last_val) for one field.

    Samples whose 'unreadable' flag is set are skipped outright. A failed
    read reports 0, and a spurious 0 is indistinguishable from the real
    transition-to-zero this tool exists to date -- so it must never enter the
    change history at all.
    """
    first_t = last_change_t = None
    first_val = last_val = None
    changes = 0
    for r in rows:
        if r.get("unreadable", 0):
            continue
        if field not in r:
            continue
        t = r.get("t", 0)
        v = r[field]
        if first_t is None:
            first_t, first_val, last_val, last_change_t = t, v, v, t
            continue
        if v != last_val:
            changes += 1
            last_change_t = t
            last_val = v
    return first_t, last_change_t, changes, first_val, last_val


def cmd_onset(records):
    """When did each watched field last move, and in what order did they stop?

    This is the query that retracted a root-cause claim on 2026-09-04. A
    suspect whose value froze at t=143 cannot explain a symptom that was
    already frozen at t=134 -- the cause has to stop moving no later than the
    effect. That check is three lines of reasoning and was skipped for weeks
    because computing the table by hand from a console log was tedious enough
    to feel optional.
    """
    rows, fields = _watch_rows(records)
    if not rows:
        print("no WATCH records in the sink.")
        print("Set PS2X_TRACE_WATCH=\"name=0xADDR,...\" and re-run; the 1 Hz")
        print("watchdog sampler writes one WATCH record per second.")
        return
    if not fields:
        print("%d WATCH record(s) but no fields -- PS2X_TRACE_WATCH parsed empty."
              % len(rows))
        return

    span_lo = rows[0].get("t", 0)
    span_hi = rows[-1].get("t", 0)
    bad = sum(1 for r in rows if r.get("unreadable", 0))
    print("WATCH: %d sample(s), t=%d..%ds, %d field(s)%s"
          % (len(rows), span_lo, span_hi, len(fields),
             ", %d with unreadable memory (excluded)" % bad if bad else ""))
    print("")

    tracks = []
    for f in fields:
        first_t, last_change_t, changes, first_val, last_val = _field_track(rows, f)
        if first_t is None:
            tracks.append((f, None, None, 0, None, None))
            continue
        tracks.append((f, first_t, last_change_t, changes, first_val, last_val))

    # Earliest freeze first: that is the onset order, and the field at the top
    # is the only one that can be upstream of everything below it.
    tracks.sort(key=lambda row: (row[2] is None, row[2] if row[2] is not None else 0))

    print("%-16s %8s %12s %8s %12s %12s  %s"
          % ("field", "first_t", "last_change", "changes", "first", "last", "verdict"))
    print("-" * 96)
    for f, first_t, last_change_t, changes, first_val, last_val in tracks:
        if first_t is None:
            print("%-16s %8s %12s %8d %12s %12s  %s"
                  % (f, "-", "-", 0, "-", "-", "NEVER SAMPLED"))
            continue
        frozen_for = span_hi - last_change_t
        if changes == 0:
            verdict = "CONSTANT for the whole window"
        elif changes == 1 and first_val == 0 and last_val != 0:
            # 0 -> value, exactly once. That is an INITIALISATION, not a
            # freeze: the field held nothing until its writer first ran, and
            # "frozen since t=N" would invite reading a startup as a stall.
            # This is not a corner case for the sofdec preset -- the CRI
            # globals are all zero at the main menu, so most of the field set
            # looks precisely like this until the movie window opens.
            verdict = ("first written at t=%d, unchanged since (%ds)"
                       " -- INIT, not a freeze" % (last_change_t, frozen_for))
        elif frozen_for >= 5:
            verdict = "frozen since t=%d (%ds)" % (last_change_t, frozen_for)
        else:
            verdict = "still moving"
        print("%-16s %8d %12d %8d %12s %12s  %s"
              % (f, first_t, last_change_t, changes,
                 "0x%x" % first_val, "0x%x" % last_val, verdict))

    print("")
    print("Read the table top-down: a field cannot be the CAUSE of anything")
    print("that froze above it. A constant field tested nothing -- it may")
    print("never have been written, or its writer may never have run.")
    print("")
    print("Rows marked INIT are 0 -> value transitions, i.e. the moment that")
    print("field's writer FIRST ran. A block of them sharing one timestamp")
    print("dates the start of a phase, not the start of a stall; only the")
    print("rows above them that genuinely stopped are onset evidence.")


def cmd_watch(records, field, limit):
    """Time series for one watched field. Changes are flagged with '*'."""
    rows, fields = _watch_rows(records)
    if not rows:
        print("no WATCH records in the sink (is PS2X_TRACE_WATCH set?).")
        return
    if field not in fields:
        print("no field %r in the WATCH records. Available: %s"
              % (field, ", ".join(fields) if fields else "(none)"))
        return

    prev = None
    shown = 0
    for r in rows:
        if field not in r:
            continue
        v = r[field]
        mark = " " if prev is None or v == prev else "*"
        flag = "  UNREADABLE" if r.get("unreadable", 0) else ""
        print("t=%-5d %s %s=0x%x%s" % (r.get("t", 0), mark, field, v, flag))
        prev = v
        shown += 1
        if limit and shown >= limit:
            break


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
    ap.add_argument("--coverage", action="store_true",
                    help="what the EE actually dispatched (needs PS2_COVERAGE=1)")
    ap.add_argument("--band", choices=("game", "sdk"),
                    help="with --coverage, restrict to one band (default: both)")
    ap.add_argument("--diff", metavar="OTHER.jsonl",
                    help="with --coverage, addresses present in one run and not the other")
    ap.add_argument("--gs", action="store_true",
                    help="GS render-path probes -- reads run_log.txt, not the sink")
    ap.add_argument("--log", default=DEFAULT_LOG,
                    help="with --gs, non-default run_log.txt location")
    ap.add_argument("--hwwatch", action="store_true",
                    help="hardware-watchpoint hits joined to their call sites")
    ap.add_argument("--dump", metavar="PATH",
                    help="with --hwwatch, non-default .hwwatch.txt location")
    ap.add_argument("--val", metavar="HEX",
                    help="with --hwwatch, only hits whose stored value matches")
    ap.add_argument("--all-runs", action="store_true",
                    help="with --hwwatch, every run in the dump, not just the last")
    ap.add_argument("--onset", action="store_true",
                    help="when each PS2X_TRACE_WATCH field last moved, earliest "
                         "freeze first (the causality-ordering check)")
    ap.add_argument("--watch", metavar="FIELD",
                    help="time series for one PS2X_TRACE_WATCH field")
    ap.add_argument("--limit", type=int, default=0, help="cap dumped records")
    ap.add_argument("--tag", metavar="NAME",
                    help="dump [NAME] records from the console log + cap verdict")
    ap.add_argument("--runs", action="store_true",
                    help="inventory every archived run: how far the movie path got")
    ap.add_argument("--archive", default=DEFAULT_ARCHIVE,
                    help="with --runs, non-default logs/archive location")
    ap.add_argument("--compare", nargs=2, metavar=("A.txt", "B.txt"),
                    help="cross-run tag diff between two console logs")
    args = ap.parse_args()

    # --runs and --compare read console logs, not the JSONL sink, so they must
    # run before the sink-existence gate below.
    if args.runs:
        return cmd_runs(os.path.abspath(args.archive), args.log)

    if args.compare:
        return cmd_compare(os.path.abspath(args.compare[0]),
                           os.path.abspath(args.compare[1]), args.limit)

    # --hwwatch needs BOTH sinks (values+cap from the JSONL, stacks from the
    # text dump), so it runs before the JSONL-only existence gate below.
    if args.hwwatch:
        sink = os.path.abspath(args.file)
        dump = os.path.abspath(args.dump or (sink + DEFAULT_HWDUMP_SUFFIX))
        val = int(args.val, 16) if args.val else None
        return cmd_hwwatch(sink, dump, val, args.all_runs, args.limit)

    # --gs reads the console log, not the JSONL sink, so it must run before the
    # sink-existence gate below or a GS-only run cannot be analysed at all.
    if args.gs:
        return cmd_gs(os.path.abspath(args.log), args.limit)

    # --tag likewise reads the console log, not the JSONL sink.
    if args.tag:
        return cmd_tag(os.path.abspath(args.log), args.tag, args.limit)

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
    elif args.onset:
        cmd_onset(records)
    elif args.watch:
        cmd_watch(records, args.watch, args.limit)
    elif args.derail:
        cmd_derail(records)
    elif args.threads:
        cmd_threads(records)
    elif args.signature:
        cmd_signature(records)
    elif args.coverage:
        cmd_coverage(records, args.band, args.limit, args.diff)
    else:
        cmd_summary(records)
    return 0


if __name__ == "__main__":
    sys.exit(main())
