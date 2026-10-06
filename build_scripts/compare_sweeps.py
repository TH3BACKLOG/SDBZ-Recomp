#!/usr/bin/env python3
"""Compare two gfx_tour runs of the SAME script (e.g. -RasterThreads 0 vs 4): speed, scene timeline, coverage.

    python build_scripts/compare_sweeps.py gsdump/rt0 gsdump/rt4

Prints per run: median vbl/s and busy% (watchdog lines), scene count, distinct logged uploads, GS captures
(report header) and oracle tally, then: first tick where the [scene] timelines differ, and the upload hashes
only one run has. The game is tick-deterministic (PS2X_DET_VBLANK_QUANTUM), so identical scripts should give
identical scenes; a different raster thread count must not change the GS stream, only the speed.
"""
import json
import re
import statistics
import sys
from pathlib import Path


def read_log(d):
    p = d / "run_log.txt"
    raw = p.read_bytes()
    return raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8", "replace")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    runs = [Path(a) for a in sys.argv[1:]]
    info = []
    for d in runs:
        text = read_log(d)
        vbl, busy, scenes = [], [], []
        for line in text.splitlines():
            if line.startswith("[watchdog]"):
                m = re.search(r"busy%=(\d+).*?vbl/s=(\d+)", line)
                if m:
                    busy.append(int(m[1]))
                    vbl.append(int(m[2]))
            elif "[scene]" in line:
                m = re.search(r"\[scene\] tick=(\d+) (app=\S+ vt=\S+ mode=\d+ p1=0x[0-9a-f]{2})", line)
                if m:
                    scenes.append((int(m[1]), m[2]))
        cov = {}
        cj = d / "coverage_hashes.json"
        if cj.exists():
            cov = json.loads(cj.read_text(encoding="utf-8"))
        rep = d / "report.md"
        head = rep.read_text(encoding="utf-8", errors="replace").splitlines()[:6] if rep.exists() else []
        info.append(dict(dir=d, vbl=vbl, busy=busy, scenes=scenes, cov=cov, head=head))
        print(f"== {d}")
        if vbl:
            print(f"  watchdog samples {len(vbl)}  median vbl/s {statistics.median(vbl)}  median busy% {statistics.median(busy)}")
        print(f"  scenes {len(scenes)}  logged complete uploads {len(cov.get('fnv', []))}  capture uploads {len(cov.get('full', []))}")
        for h in head:
            if h.startswith("- captures") or h.startswith("- **BROKEN") or h.startswith("- oracle"):
                print("  " + h)
    a, b = info
    n = min(len(a["scenes"]), len(b["scenes"]))
    diff = next((i for i in range(n) if a["scenes"][i] != b["scenes"][i]), None)
    print("== scene timelines")
    if diff is None:
        print(f"  identical over the first {n} scene changes")
    else:
        print(f"  first difference at scene #{diff}: {a['scenes'][diff]}  vs  {b['scenes'][diff]}")
    for key in ("fnv", "full"):
        sa, sb = set(a["cov"].get(key, [])), set(b["cov"].get(key, []))
        print(f"== {key}: only in {a['dir'].name}: {len(sa - sb)}   only in {b['dir'].name}: {len(sb - sa)}   common: {len(sa & sb)}")


main()
