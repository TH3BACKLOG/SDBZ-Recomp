#!/usr/bin/env python3
"""pcsx2_grade.py -- grade the GS dumps of a PCSX2 sweep (gfx_tour.ps1 -Emu pcsx2).

For every cap_t<tick>.gs.zst this runs gfx_scene_diff.py's single-dump mode: OUR
rasterizer replays PCSX2's GS stream and is compared with PCSX2's picture of the same
stream, and every differing region is blamed on a draw.

Reference = PCSX2's SOFTWARE renderer on the same dump (GSRunner `-renderer sw -dump f`),
vs OUR replay stopped at the last complete frame (see sw_reference): same frame, same size. The
screenshot PCSX2 saved next to the dump (cap_t<tick>.png) is only a fallback: it is the
hardware render at window size, resized, and it is taken one frame BEFORE the dump starts,
so fades and movies differ by a frame (pc_smoke 10-08: 9 false FLAGs in the intro movie,
all gone against the GSRunner frame). Only regions where ours is clearly darker (missing)
or brighter (extra) are FLAGGED.

    python build_scripts/pcsx2_grade.py gsdump/<name>            -> gsdump/<name>/pcsx2_report.md
    python build_scripts/pcsx2_grade.py gsdump/<name> --jobs 4
"""
import argparse
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gfx_scene_diff as gsd  # noqa: E402  (read_scenes / scene_label_at)

MEAN_RE = re.compile(r"^\[3\] mean abs diff ([\d.]+); (\d+) differing region")
REGION_RE = re.compile(r"^(region#\d+) \((\d+),(\d+)\) (\d+)x(\d+)\s+diff=(\d+) lum ours=(\d+) ref=(\d+) -> (.*)$")
VERDICT_RE = re.compile(r"^    (last pixel-changing draw: .*|NO DRAW.*|NO EFFECT.*)$")


def tick_of(path):
    m = re.search(r"_t(\d+)", path.name)
    return int(m.group(1)) if m else -1


def sw_reference(dump, out):
    """PCSX2 software render of this dump -> (ref png, --fbp, --stop, note), or (None, None, None, why).

    A dump ends mid-frame. GSRunner only writes finished frames, but OUR full replay also draws the
    unfinished one, and when it lands in the displayed buffer ours = old frame + half a new one
    (pc_vsall3 10-08: 3 false FLAGs in fights; at frame ends ours matched PCSX2 to 0.65). So OUR side
    stops at last_complete_frame() (same rule as the tour sheet), and the reference is the PCSX2 frame
    of that buffer closest to it (frames of one buffer are 2 vsyncs apart, so a real raster bug still
    differs from all of them)."""
    runner = gsd.find_gsrunner()
    geo = gsd.display_geometry(dump, out)
    if not runner or not runner.exists() or not geo:
        return None, None, None, "no GSRunner or no display regs"
    dfbp, fbw, w, h = geo
    gsr = gsd.prepare(dump, out)  # out/frame.gsr; gfx_scene_diff reuses it
    lcf = gsd.last_complete_frame(gsr, fbw, w, h)
    fbp, stop = (lcf[0], lcf[1]) if lcf else (dfbp, None)
    rt = out / "sw"
    rt.mkdir(exist_ok=True)
    for old in rt.glob("*"):
        old.unlink()
    rc, _ = gsd.run([str(runner), "-renderer", "sw", "-dump", "f", "-dumpdir", str(rt.resolve()), "-loop", "1",
                     "-surfaceless", "-noshadercache", "--", str(dump.resolve())], {"PCSX2_NOCONSOLE": "1"}, timeout=600)
    # <draw>_f<frame>_fr-1_<FBP in blocks, hex>_<psm>.png ; last draw file of each frame in that buffer
    want = f"_{fbp * 32:05x}_"
    last = {}
    for f in rt.glob("*_f*.png"):
        m = re.match(r"(\d+)_f(\d+)_", f.name)
        if m and want in f.name and (int(m.group(2)) not in last or int(m.group(1)) > last[int(m.group(2))][0]):
            last[int(m.group(2))] = (int(m.group(1)), f)
    ref, note = None, "no SW frame for the buffer"
    if last:
        pick = max(last)
        if stop is not None and len(last) > 1:
            ours = out / "ours_lcf.bmp"
            gsd.render(gsr, ours, fbp, fbw, {"PS2X_GSBENCH_STOP": str(stop)}, w, h)
            o = np.asarray(gsd.load(ours), dtype=np.int16)
            ours.unlink()

            def dist(fr):
                img = gsd.load(last[fr][1])
                if img.size[0] < w or img.size[1] < h:
                    return float("inf")
                return float(np.abs(o - np.asarray(img.crop((0, 0, w, h)), dtype=np.int16)).mean())
            pick = min(last, key=dist)
        ref = out / "pcsx2_sw.png"
        last[pick][1].replace(ref)
        note = f"f{pick} of {len(last)}"
    for f in rt.glob("*"):
        f.unlink()
    rt.rmdir()
    return ref, fbp, stop, note


def grade(dump, out_root):
    stem = dump.name.split(".gs")[0]
    out = out_root / stem
    out.mkdir(parents=True, exist_ok=True)
    ref, fbp, stop, note = sw_reference(dump, out)
    cmd = [sys.executable, str(HERE / "gfx_scene_diff.py"), str(dump), "--out", str(out)]
    if ref:
        cmd += ["--ref", str(ref), "--fbp", hex(fbp)]
        if stop is not None:
            cmd += ["--stop", str(stop)]
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    (out / "grade.txt").write_text(text, encoding="utf-8")
    for big in ("frame.gsr", "frame.vram", "regs.bin", "ours.bmp"):  # ~12 MB a capture, all rebuilt on a re-grade
        (out / big).unlink(missing_ok=True)
    res = {"stem": stem, "tick": tick_of(dump), "rc": proc.returncode, "mean": None, "regions": [], "out": out,
           "ref": f"sw {note}" if ref else "png"}
    cur = None
    for line in text.splitlines():
        m = MEAN_RE.match(line)
        if m:
            res["mean"] = float(m.group(1))
            res["nregions"] = int(m.group(2))
            continue
        m = REGION_RE.match(line)
        if m:
            cur = {"name": m.group(1), "rect": tuple(int(m.group(i)) for i in range(2, 6)), "diff": int(m.group(6)),
                   "ours": int(m.group(7)), "ref": int(m.group(8)), "kind": m.group(9), "blame": ""}
            res["regions"].append(cur)
            continue
        m = VERDICT_RE.match(line)
        if m and cur is not None and not cur["blame"]:
            cur["blame"] = m.group(1)
    flagged = [r for r in res["regions"] if "missing" in r["kind"] or "extra" in r["kind"]]
    res["flagged"] = flagged
    if proc.returncode != 0 or res["mean"] is None:
        res["verdict"] = "ERROR"
    else:
        res["verdict"] = "FLAG" if flagged else "MATCH"
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path)
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    folder = a.folder
    dumps = sorted((p for p in folder.glob("cap_t*.gs*") if re.search(r"\.gs(\.zst|\.xz)?$", p.name)), key=tick_of)
    if not dumps:
        sys.exit(f"no cap_t*.gs.zst dumps in {folder}")
    scenes = gsd.read_scenes(folder)
    out_root = folder / "pcsx2_cmp"
    print(f"grading {len(dumps)} PCSX2 dump(s) on {a.jobs} worker(s) ...")
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        results = list(ex.map(lambda d: grade(d, out_root), dumps))
    counts = {k: sum(r["verdict"] == k for r in results) for k in ("MATCH", "FLAG", "ERROR")}
    lines = [f"# PCSX2 sweep grade: {folder.name}", "",
             f"{len(results)} dump(s): MATCH {counts['MATCH']}, FLAG {counts['FLAG']}, ERROR {counts['ERROR']}", "",
             "Reference (ref column): sw fN = PCSX2's software renderer on the same dump, frame N of the buffer, vs OUR",
             "replay stopped at the last complete frame (the dump ends mid-frame); png = PCSX2's",
             "screenshot (fallback: hardware render, resized, one frame early). FLAG = a region where our replay is",
             "clearly darker (missing?) or brighter (extra?) than PCSX2.",
             "Per capture: `pcsx2_cmp/<capture>/compare.png` (ours | PCSX2 | diff) and `grade.txt`.", "",
             "| capture | scene | verdict | ref | mean diff | regions | flagged |", "|---|---|---|---|---|---|---|"]
    for r in results:
        scene = gsd.scene_label_at(scenes, r["tick"]) if scenes else "unknown"
        mean = f"{r['mean']:.2f}" if r["mean"] is not None else "-"
        lines.append(f"| {r['stem']} | {scene} | {r['verdict']} | {r['ref']} | {mean} | {len(r['regions'])} | {len(r['flagged'])} |")
    for r in results:
        if r["verdict"] == "MATCH":
            continue
        lines += ["", f"## {r['stem']} ({r['verdict']})"]
        if r["verdict"] == "ERROR":
            lines.append(f"gfx_scene_diff rc={r['rc']}; see `pcsx2_cmp/{r['stem']}/grade.txt`")
        for g in r["flagged"]:
            x, y, w, h = g["rect"]
            lines.append(f"- {g['name']} ({x},{y}) {w}x{h} lum ours={g['ours']} pcsx2={g['ref']} -> {g['kind']}; {g['blame']}")
    rep = folder / "pcsx2_report.md"
    rep.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"MATCH {counts['MATCH']}  FLAG {counts['FLAG']}  ERROR {counts['ERROR']}  -> {rep}")


if __name__ == "__main__":
    main()
