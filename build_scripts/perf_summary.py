#!/usr/bin/env python3
"""perf_summary.py -- one-screen perf summary of a run_log.txt.

Reads the per-second [watchdog] lines (t=, busy%=, vbl/s=) and reports the
mean/median/min/max over an ACTIVE window, so runs can be compared without
pasting the log. Encoding-safe (reuses analyze_run.read_text_any).

  python build_scripts/perf_summary.py                     # run_log.txt, t=80..240
  python build_scripts/perf_summary.py LOG --from 80 --to 240
  python build_scripts/perf_summary.py A.txt B.txt         # side by side

Also prints the last [gsraster-wait] / [cputime] record for context.
"""
import argparse
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_run import DEFAULT_LOG, read_text_any  # noqa: E402

WD = re.compile(r"\[watchdog\] t=(\d+)s.*?busy%=(\d+).*?vbl/s=(\d+)")


def summarize(path, lo, hi):
    if not os.path.exists(path):
        print("log not found: %s" % path)
        return None
    text = read_text_any(path)
    rows = [(int(t), int(b), int(v)) for t, b, v in WD.findall(text)]
    win = [r for r in rows if lo <= r[0] <= hi]
    print("== %s  (%d watchdog lines, %d in t=%d..%d)" % (path, len(rows), len(win), lo, hi))
    if not win:
        print("   NO watchdog lines in window -- do not read this as 0 vbl/s.")
        return None
    vbl = [r[2] for r in win]
    busy = [r[1] for r in win]
    out = {
        "vbl_mean": statistics.mean(vbl), "vbl_med": statistics.median(vbl),
        "vbl_min": min(vbl), "vbl_max": max(vbl), "busy_mean": statistics.mean(busy),
    }
    print("   vbl/s  mean=%.1f median=%.1f min=%d max=%d   busy%% mean=%.1f"
          % (out["vbl_mean"], out["vbl_med"], out["vbl_min"], out["vbl_max"], out["busy_mean"]))
    for tag in ("gsraster-wait", "cputime"):
        recs = [m for m in re.findall(r"\[%s\][^\[]*" % re.escape(tag), text)]
        if recs:
            print("   last [%s]: %s" % (tag, " ".join(recs[-1].split())[:300]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", default=[DEFAULT_LOG])
    ap.add_argument("--from", dest="lo", type=int, default=80)
    ap.add_argument("--to", dest="hi", type=int, default=240)
    a = ap.parse_args()
    res = [summarize(p, a.lo, a.hi) for p in a.logs]
    if len(res) == 2 and all(res):
        d = res[1]["vbl_mean"] / res[0]["vbl_mean"] - 1.0
        print("\nB vs A: vbl/s mean %+.1f%%" % (d * 100.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
