#!/usr/bin/env python3
"""Put our run next to PCSX2, field for field, and say where they diverge.

WHY THIS EXISTS
---------------
The most expensive rule in this project is "reproduce on the oracle before
naming a root cause". It has been broken at least twice, and each time it cost
a headline conclusion:

  * part 58 -- a fully measured, internally consistent mechanism, retracted
    once hardware was shown to do the same thing.
  * part 66 -- a circular-deadlock story, same ending.

Neither failure was an analysis failure. Both were caused by the check being
manual: our numbers came out of a console log, the oracle's out of a CSV, the
field names were maintained in two places, and lining them up by hand was
tedious enough that "it's obviously wrong, look at it" won every time.

This script makes the check cost one command. It is deliberately blunt: it
does not try to explain anything, it only answers "is this field's behaviour
ours, or is it the game's?".

INPUTS
------
  --ours    run_probe.jsonl written by our runtime with PS2X_TRACE_WATCH set
            (probe=WATCH records; see analyze_run.py --onset)
  --oracle  CSV written by pcsx2_sampler.py over the same preset

Both sides are keyed by FIELD NAME, never by column index -- the two samplers
run at different rates and start at different moments, so positional alignment
would silently compare unrelated things. presets.py is what keeps the names
identical on both sides.

VERDICTS
--------
  DIVERGE      the field moves on one side and is frozen on the other, or the
               rates differ by more than --rate-factor. This is the only
               verdict that points at us.
  DIVERGE also covers disjoint VALUE SETS on flag/enum-shaped fields: both
               sides move, but through states that never overlap.
  ours-init    our window opened before the movie and the oracle's did not,
               so the field's first write shows here and not there. Both sides
               end on the same value: a window artifact, not a finding.
  both-move    both sides advance. Whatever this field measures, it is not the
               difference between a working game and ours.
  both-frozen  neither side moves. Usually means the window is wrong (sample
               during the movie, not after it) -- NOT evidence of a bug.
  ours-only / oracle-only
               the field is missing from one input entirely.

EXIT CODES
----------
  0  at least one field diverged (there is something here to chase)
  1  nothing diverged -- our behaviour matches hardware on every field, so
     whatever you were about to blame, this evidence does not support it
  2  inputs unusable (missing file, no overlap, empty)

The exit code is the point. "Nothing diverged" is a real, useful, and
frequently unwelcome answer, and it should be as loud as a finding.
"""

import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from presets import PRESETS, fields  # noqa: E402

META = ("seq", "tid", "progress", "probe", "t", "unreadable")


def load_ours(path):
    """WATCH records -> {field: [values in time order]} plus the t span.

    Samples flagged unreadable are dropped: a failed guest read reports 0, and
    a spurious 0 is exactly the shape of the transition this tool hunts.
    """
    series, span = {}, []
    with open(path, "r", encoding="ascii", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                raw = json.loads(line)
            except ValueError:
                continue  # truncated tail line after a timeout kill; expected
            if raw.get("probe") != "WATCH":
                continue
            try:
                if int(raw.get("unreadable", "0x0"), 16):
                    continue
                t = int(raw.get("t", "0x0"), 16)
            except (ValueError, TypeError):
                continue
            span.append(t)
            for k, v in raw.items():
                if k in META:
                    continue
                try:
                    series.setdefault(k, []).append(int(v, 16))
                except (ValueError, TypeError):
                    pass
    return series, span


def load_oracle(path):
    """pcsx2_sampler CSV -> {field: [values]} plus the timestamp span.

    Accepts either a 't' or a 'time' column; anything non-numeric in a cell is
    skipped rather than coerced, because a coerced sample is a fabricated one.
    """
    series, span, skipped, unflagged = {}, [], 0, 0
    with open(path, "r", encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            tcol = row.get("t", row.get("t_ms", row.get("time", "")))

            vals = {}
            for k, v in row.items():
                if k is None or k in ("t", "t_ms", "time", "status", "unreadable"):
                    continue
                if v is None or v == "":
                    continue
                try:
                    vals[k] = int(str(v), 0)
                except ValueError:
                    pass

            # Trust the sampler's flag, and nothing else. An earlier version
            # inferred a dead read from "every field is 0", which is wrong in
            # both directions: before the movie opens those fields really are
            # all zero (160 good samples were thrown away that way), and a
            # dead read can land while some cached value is still nonzero.
            # pcsx2_sampler.py settles it with a sentinel read of guest code
            # in the same packet. A CSV with no such column predates that and
            # gets a warning, not a guess.
            if "unreadable" not in row:
                unflagged += 1
            dead = str(row.get("unreadable", "0")).strip() not in ("", "0")
            if not dead and row.get("status") not in (None, "", "Running"):
                dead = True
            if dead:
                skipped += 1
                continue

            try:
                # t_ms is milliseconds; everything downstream is seconds, and
                # a 1000x window is not a unit nit -- the rate test divides by
                # it.
                t = float(tcol)
                span.append(t / 1000.0 if "t_ms" in row else t)
            except (TypeError, ValueError):
                pass
            for k, v in vals.items():
                series.setdefault(k, []).append(v)

    if unflagged:
        sys.stderr.write(
            ("oracle: %d row(s) predate the sentinel check -- dead reads in "
             "this CSV are indistinguishable from cleared fields. Re-capture "
             "before trusting a 'both-frozen'." % unflagged) + chr(10))
    if skipped:
        sys.stderr.write(
            ("oracle: dropped %d unreadable sample(s) (paused, or sentinel 0)"
             % skipped) + chr(10))
    return series, span


def describe(values):
    """(changes, first, last, distinct) for one field's samples."""
    if not values:
        return 0, None, None, 0
    changes = sum(1 for a, b in zip(values, values[1:]) if a != b)
    return changes, values[0], values[-1], len(set(values))


def counter_step(values, duration=None):
    """Mean per-sample increment if this looks like a monotonic counter.

    Returns None for anything that ever goes backwards -- a flag or a handle
    has no meaningful "rate", and pretending otherwise would manufacture a
    divergence out of two unrelated bit patterns.

    This exists because "does it move?" is the wrong question for a counter.
    Our class-6 worker tick and hardware's BOTH move; the finding is that ours
    moves ~1700x faster. A change-rate comparison saturates at 1.0 on each
    side and reports 'both-move', which is how a real 1700x gap can hide in a
    diff that looks clean.
    """
    if len(values) < 2:
        return None
    # A field that only ever held a handful of values is a flag or a state
    # enum, not a counter. 0 -> 1 is monotonic, but "+0.0127/s" is a fiction,
    # and inviting a rate comparison on it manufactures divergences.
    if len(set(values)) <= 4:
        return None

    # Measure the LONGEST non-decreasing run rather than bailing on the first
    # backward step. A counter that restarts has not stopped being a counter:
    # the 2026-09-05 oracle capture spanned a PCSX2 reset, w6tick went
    # 1329 -> 3, and the old all-or-nothing test returned None -- which
    # skipped the rate comparison entirely and printed the known ~1700x gap
    # between us and hardware as an unremarkable "both-move".
    best = cur = (0, 0)
    for i in range(1, len(values)):
        cur = (cur[0], i) if values[i] >= values[i - 1] else (i, i)
        if cur[1] - cur[0] > best[1] - best[0]:
            best = cur
    lo, hi = best
    if hi - lo < 2:
        return None

    span_n = hi - lo
    delta = values[hi] - values[lo]
    if duration and len(values) > 1:
        # Per SECOND. Per-sample would silently scale by the ratio of the two
        # sampling rates -- ours 1Hz, PCSX2 10Hz -- putting a factor of 10
        # into a comparison whose whole job is to judge a ratio.
        per_sample_secs = duration / float(len(values) - 1)
        return delta / (span_n * per_sample_secs)
    return delta / float(span_n)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="sofdec", choices=sorted(PRESETS),
                    help="field set to compare (default: sofdec)")
    ap.add_argument("--ours", default="run_probe.jsonl",
                    help="our probe sink (WATCH records)")
    ap.add_argument("--oracle", required=True,
                    help="pcsx2_sampler.py CSV from a running PCSX2")
    ap.add_argument("--rate-factor", type=float, default=10.0,
                    help="per-sample change rates differing by more than this "
                         "multiple count as DIVERGE (default: 10)")
    ap.add_argument("--all", action="store_true",
                    help="show every field, not just the divergent ones")
    args = ap.parse_args()

    for path, what in ((args.ours, "ours"), (args.oracle, "oracle")):
        if not os.path.exists(path):
            sys.stderr.write("%s input not found: %s\n" % (what, path))
            return 2

    ours, ours_span = load_ours(args.ours)
    oracle, oracle_span = load_oracle(args.oracle)

    if not ours:
        sys.stderr.write(
            "no WATCH records in %s.\n"
            "Set PS2X_TRACE_WATCH before the run:\n"
            "    python build_scripts/presets.py %s\n" % (args.ours, args.preset))
        return 2
    if not oracle:
        sys.stderr.write("no usable rows in %s\n" % args.oracle)
        return 2

    # Report both windows rather than assuming they line up. Two captures of
    # the same field taken in different phases of the boot are not a
    # comparison, and silently treating them as one is how a diff tool starts
    # producing confident nonsense.
    def span_str(span):
        return "%g..%g (%d samples)" % (min(span), max(span), len(span)) \
            if span else "unknown"

    print("preset : %s" % args.preset)
    print("ours   : %s   %s" % (args.ours, span_str(ours_span)))
    print("oracle : %s   %s" % (args.oracle, span_str(oracle_span)))
    print("")
    print("Windows are reported, not checked for overlap -- the two clocks are")
    print("unrelated (our t is watchdog seconds, PCSX2's is wall time). Confirm")
    print("by hand that BOTH captures cover the movie window before believing")
    print("a 'both-frozen'.")
    print("")

    names = [name for name, _addr, _w in fields(args.preset)]
    for extra in list(ours) + list(oracle):
        if extra not in names:
            names.append(extra)

    rows, diverged = [], 0
    for name in names:
        o_vals, h_vals = ours.get(name), oracle.get(name)
        if not o_vals and not h_vals:
            continue
        if not o_vals:
            rows.append((name, "oracle-only", "", ""))
            continue
        if not h_vals:
            rows.append((name, "ours-only", "", ""))
            continue

        o_ch, o_first, o_last, _ = describe(o_vals)
        h_ch, h_first, h_last, _ = describe(h_vals)
        # Rate per sample, so the two sampling frequencies cancel out.
        o_rate = o_ch / float(len(o_vals))
        h_rate = h_ch / float(len(h_vals))

        o_dur = (max(ours_span) - min(ours_span)) if ours_span else None
        h_dur = (max(oracle_span) - min(oracle_span)) if oracle_span else None
        o_step = counter_step(o_vals, o_dur)
        h_step = counter_step(h_vals, h_dur)

        # A field that starts at 0 here, ends where the oracle sits, and moved
        # exactly once is our capture watching its FIRST write -- our window
        # opens before the movie, PCSX2's opened inside it. That is a window
        # difference, not a behavioural one, and calling it DIVERGE buries the
        # one field that really does differ. Same init-vs-freeze trap that
        # analyze_run.py --onset had to be taught.
        init_only = (o_ch == 1 and o_first == 0 and o_last == h_last
                     and h_ch == 0 and h_last != 0)

        # Value-set check, for enum/flag-shaped fields only. Two sides can
        # both "move" while holding values that never overlap -- h48 is 1 in
        # our runtime and takes only {0,4,6} on hardware, so the completion
        # predicate is reading a state the real game never produces. A rate
        # comparison is blind to that, and it is a stronger statement than a
        # rate gap: not "too fast", but "never happens".
        o_set, h_set = set(o_vals) - {0}, set(h_vals) - {0}
        disjoint_values = (o_set and h_set and not (o_set & h_set)
                           and len(o_set) <= 8 and len(h_set) <= 8)

        if init_only:
            verdict = "ours-init"
        elif disjoint_values:
            verdict = "DIVERGE"
        elif (o_ch == 0) != (h_ch == 0):
            verdict = "DIVERGE"
        elif o_ch == 0 and h_ch == 0:
            verdict = "both-frozen"
        elif o_step is not None and h_step is not None and \
                min(o_step, h_step) > 0 and \
                max(o_step, h_step) / min(o_step, h_step) > args.rate_factor:
            # Both counters advance, but at wildly different speeds. Checked
            # BEFORE the change-rate test, which would call this both-move.
            verdict = "DIVERGE"
        else:
            hi, lo = max(o_rate, h_rate), min(o_rate, h_rate)
            verdict = "DIVERGE" if lo > 0 and hi / lo > args.rate_factor \
                else "both-move"

        if verdict == "DIVERGE":
            diverged += 1

        def cell(ch, first, last, step):
            base = "chg=%d 0x%x->0x%x" % (ch, first, last)
            return base + (" +%.4g/s" % step if step else "")

        rows.append((name, verdict,
                     cell(o_ch, o_first, o_last, o_step),
                     cell(h_ch, h_first, h_last, h_step)))

    shown = [r for r in rows if args.all or r[1] == "DIVERGE"]
    if not shown:
        shown = rows  # nothing diverged: show everything rather than nothing

    print("%-12s %-12s %-34s %s" % ("field", "verdict", "ours", "oracle"))
    print("-" * 104)
    for name, verdict, o, h in shown:
        print("%-12s %-12s %-34s %s" % (name, verdict, o, h))

    print("")
    if diverged:
        print("%d field(s) DIVERGE -- those are ours to explain." % diverged)
        return 0

    print("NOTHING DIVERGED.")
    print("Every compared field behaves the same on hardware as it does here.")
    print("Whatever mechanism you were about to name as the root cause, this")
    print("evidence does not support it -- hardware does the same thing and")
    print("the game works. Widen the field set or move the capture window.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
