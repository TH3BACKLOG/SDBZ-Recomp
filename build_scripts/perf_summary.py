#!/usr/bin/env python3
"""perf_summary.py -- one-screen perf summary of a run_log.txt.

Reads the per-second [watchdog] lines (t=, busy%=, vbl/s=) and reports the
mean/median/min/max over an ACTIVE window, so runs can be compared without
pasting the log. Encoding-safe (reuses analyze_run.read_text_any).

  python build_scripts/perf_summary.py                     # run_log.txt, t=80..240
  python build_scripts/perf_summary.py LOG --from 80 --to 240
  python build_scripts/perf_summary.py A.txt B.txt         # side by side

Also prints the last [gsraster-wait] / [cputime] record for context.

  python build_scripts/perf_summary.py --fight [LOG]       # real-fight report

--fight: only seconds whose [budget] vu1Us/vbl >= --vu1-min (5000 = fight-level
VU1 load, menus drop out), median/p10/p90 vbl/s, [budget] medians, per-thread
CPU and the GameThread [hostprof] leaf + owner rows. Pair with
`launch_recomp.ps1 -PerfFight`, which calls this at the end of the run.
"""
import argparse
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_run import DEFAULT_LOG, read_text_any  # noqa: E402

WD = re.compile(r"\[watchdog\] t=(\d+)s.*?busy%=(\d+).*?vbl/s=(\d+)")
# 10-09 [budget] line (ps2_runtime.cpp watchdog): per-vblank cost figures.
BUDGET_KEYS = ("xfer/vbl", "unwindUs/vbl", "vu1Us/vbl", "vu1RecompUs/vbl", "vu1KickUs/vbl",
               "vu1Runs/vbl", "rastKpx/vbl", "tris/vbl", "sprites/vbl", "abeKpx/vbl", "zKpx/vbl",
               "gifCopies/vbl")
BD = re.compile(r"\[budget\] t=(\d+) vbl/s=(\d+)([^\r\n]*)")


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
    # [budget]: median of each per-vblank figure over the window, seconds with vbl/s >= 5 only
    # (idle seconds divide by ~0 and are noise).
    brows = []
    for bt, bv, rest in BD.findall(text):
        if lo <= int(bt) <= hi and int(bv) >= 5:
            row = {}
            for k in BUDGET_KEYS:
                m = re.search(re.escape(k) + r"=(\d+)", rest)
                if m:
                    row[k] = int(m.group(1))
            brows.append(row)
    if brows:
        print("   [budget] medians per vblank over %d active seconds:" % len(brows))
        print("     " + "  ".join("%s=%d" % (k, statistics.median([r[k] for r in brows if k in r]))
                                  for k in BUDGET_KEYS if any(k in r for r in brows)))
    for tag in ("gsraster-wait", "cputime"):
        recs = [m for m in re.findall(r"\[%s\][^\[]*" % re.escape(tag), text)]
        if recs:
            print("   last [%s]: %s" % (tag, " ".join(recs[-1].split())[:300]))
    return out


def _pct(sorted_vals, p):
    # Nearest-rank percentile on an already sorted list.
    i = max(0, min(len(sorted_vals) - 1, int(round(p / 100.0 * (len(sorted_vals) - 1)))))
    return sorted_vals[i]


HP_THREAD = re.compile(r"^\[hostprof\]\s+tid (\d+)\s+([\d.]+)s\s+(.+)$")
HP_SECTION = re.compile(r"-- tid \d+ \(([^)]+)\)")
HP_LEAF = re.compile(r"^\[hostprof\]\s+([\d.]+)%\s+([\d.]+)s\s+(.+)$")
HP_OWNER = re.compile(r"^\[hostprof\]\s+owner\s+([\d.]+)%\s+(.+)$")


def fight(path, vu1_min, thread, top):
    if not os.path.exists(path):
        print("log not found: %s" % path)
        return 1
    text = read_text_any(path)
    rows = []
    for bt, bv, rest in BD.findall(text):
        m = re.search(r"vu1Us/vbl=(\d+)", rest)
        if m and int(m.group(1)) >= vu1_min:
            row = {"t": int(bt), "vbl": int(bv)}
            for k in BUDGET_KEYS:
                mk = re.search(re.escape(k) + r"=(\d+)", rest)
                if mk:
                    row[k] = int(mk.group(1))
            rows.append(row)
    print("== %s  --fight (vu1Us/vbl >= %d)" % (path, vu1_min))
    if not rows:
        print("   NO fight seconds -- the run never reached a fight; do not read this as 0 vbl/s.")
        return 1
    v = sorted(r["vbl"] for r in rows)
    print("   fight seconds=%d (t=%d..%d)  vbl/s median=%.1f p10=%d p90=%d mean=%.1f min=%d max=%d"
          % (len(v), rows[0]["t"], rows[-1]["t"], statistics.median(v), _pct(v, 10), _pct(v, 90),
             statistics.mean(v), v[0], v[-1]))
    print("   [budget] medians: " + "  ".join(
        "%s=%d" % (k, statistics.median([r[k] for r in rows if k in r]))
        for k in BUDGET_KEYS if any(k in r for r in rows)))
    # [hostprof]: records can be glued onto other output, so cut at each tag.
    next_tag = re.compile(r"\[[A-Za-z][\w:.-]*\]")
    hp = []
    for r in re.findall(r"\[hostprof\][^\r\n]*", text):
        m = next_tag.search(r, 10)
        hp.append(r[:m.start()] if m else r)
    threads = [m.groups() for m in (HP_THREAD.match(r.rstrip()) for r in hp) if m]
    if threads:
        print("   thread CPU: " + "  ".join("%s=%ss" % (n, s) for _, s, n in threads[:7]))
    sect, leaves, owners = None, [], []
    for r in hp:
        r = r.rstrip()
        ms = HP_SECTION.search(r)
        if ms:
            sect = ms.group(1)
            continue
        if sect != thread:
            continue
        ml, mo = HP_LEAF.match(r), HP_OWNER.match(r)
        if ml:
            leaves.append(ml.groups())
        elif mo:
            owners.append(mo.groups())
    if owners:
        print("   %s owners: %s" % (thread, "  ".join("%s %s%%" % (o.split("!")[-1], p) for p, o in owners[:8])))
    for p, s, name in leaves[:top]:
        print("     %5s%%  %7ss  %s" % (p, s, name[:110]))
    if not leaves:
        print("   (no [hostprof] %s section -- was -HostProfile on and did the report land before the stop?)" % thread)
    gs = re.findall(r"\[gsthread\] submits=[^\r\n]*", text)
    if gs:
        print("   last " + gs[-1][:260])
    # [p5a] (PS2X_P5A=1): VU1-thread go/no-go counters, fight seconds only.
    fight_t = {r["t"] for r in rows}
    p5 = []
    for pt, rest in re.findall(r"\[p5a\] t=(\d+)([^\r\n]*)", text):
        if int(pt) in fight_t:
            p5.append({k: int(v) for k, v in re.findall(r"([\w/]+)=(\d+)", rest)})
    if p5:
        keys = ("vif1Rd/s", "vif1Wr/s", "vpuStat/s", "gsSync/s", "d1Kicks/s", "frameUs",
                "kickUs/vbl", "outsideUs/vbl", "postKickUs/vbl")
        print("   [p5a] medians over %d fight seconds: " % len(p5) + "  ".join(
            "%s=%d" % (k, statistics.median([r[k] for r in p5 if k in r])) for k in keys if any(k in r for r in p5)))
        print("   [p5a] max: " + "  ".join(
            "%s=%d" % (k, max(r.get(k, 0) for r in p5)) for k in ("vif1Rd/s", "vif1Wr/s", "vpuStat/s", "gsSync/s")))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", default=[DEFAULT_LOG])
    ap.add_argument("--fight", action="store_true", help="real-fight report (see module doc)")
    ap.add_argument("--vu1-min", type=int, default=5000, help="--fight: min vu1Us/vbl for a fight second")
    ap.add_argument("--thread", default="GameThread", help="--fight: [hostprof] thread section to list")
    ap.add_argument("--top", type=int, default=20, help="--fight: leaf rows to print")
    ap.add_argument("--from", dest="lo", type=int, default=80)
    ap.add_argument("--to", dest="hi", type=int, default=240)
    ap.add_argument("--hostprof", action="store_true",
                    help="print only the [hostprof] report lines (startswith filter)")
    a = ap.parse_args()
    if a.fight:
        return max(fight(p, a.vu1_min, a.thread, a.top) for p in a.logs)
    if a.hostprof:
        for p in a.logs:
            for ln in read_text_any(p).splitlines():
                if ln.startswith("[hostprof]"):
                    print(ln)
        return 0
    res = [summarize(p, a.lo, a.hi) for p in a.logs]
    if len(res) == 2 and all(res):
        d = res[1]["vbl_mean"] / res[0]["vbl_mean"] - 1.0
        print("\nB vs A: vbl/s mean %+.1f%%" % (d * 100.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
