#!/usr/bin/env python3
"""pcsx2_grade.py -- grade the GS dumps of a PCSX2 sweep (gfx_tour.ps1 -Emu pcsx2).

Each cap_t<tick>.gs.zst comes with the screenshot PCSX2 took of the same frame
(cap_t<tick>.png). For every pair this runs gfx_scene_diff.py's single-dump mode:
OUR rasterizer replays PCSX2's GS stream and is compared with PCSX2's own picture,
and every differing region is blamed on a draw.

The screenshot is PCSX2's hardware render at window size, resized to the PS2 frame,
so edges always differ a little (a matching fight: mean diff ~3.8, regions marked
"different content" with lum within 5). Only regions where ours is clearly darker
(missing) or brighter (extra) are FLAGGED.

    python build_scripts/pcsx2_grade.py gsdump/<name>            -> gsdump/<name>/pcsx2_report.md
    python build_scripts/pcsx2_grade.py gsdump/<name> --jobs 4
"""
import argparse
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gfx_scene_diff as gsd  # noqa: E402  (read_scenes / scene_label_at)

MEAN_RE = re.compile(r"^\[3\] mean abs diff ([\d.]+); (\d+) differing region")
REGION_RE = re.compile(r"^(region#\d+) \((\d+),(\d+)\) (\d+)x(\d+)\s+diff=(\d+) lum ours=(\d+) ref=(\d+) -> (.*)$")
VERDICT_RE = re.compile(r"^    (last pixel-changing draw: .*|NO DRAW.*|NO EFFECT.*)$")


def tick_of(path):
    m = re.search(r"_t(\d+)", path.name)
    return int(m.group(1)) if m else -1


def grade(dump, out_root):
    stem = dump.name.split(".gs")[0]
    out = out_root / stem
    out.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run([sys.executable, str(HERE / "gfx_scene_diff.py"), str(dump), "--out", str(out)],
                          capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    (out / "grade.txt").write_text(text, encoding="utf-8")
    for big in ("frame.gsr", "frame.vram", "regs.bin", "ours.bmp"):  # ~12 MB a capture, all rebuilt on a re-grade
        (out / big).unlink(missing_ok=True)
    res = {"stem": stem, "tick": tick_of(dump), "rc": proc.returncode, "mean": None, "regions": [], "out": out}
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
    dumps = [d for d in dumps if d.with_name(d.name.split(".gs")[0] + ".png").exists()]
    if not dumps:
        sys.exit(f"no cap_t*.gs.zst + .png pairs in {folder}")
    scenes = gsd.read_scenes(folder)
    out_root = folder / "pcsx2_cmp"
    print(f"grading {len(dumps)} PCSX2 dump(s) on {a.jobs} worker(s) ...")
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        results = list(ex.map(lambda d: grade(d, out_root), dumps))
    counts = {k: sum(r["verdict"] == k for r in results) for k in ("MATCH", "FLAG", "ERROR")}
    lines = [f"# PCSX2 sweep grade: {folder.name}", "",
             f"{len(results)} dump(s): MATCH {counts['MATCH']}, FLAG {counts['FLAG']}, ERROR {counts['ERROR']}", "",
             "Reference = PCSX2's own screenshot of the dumped frame (hardware render, resized). FLAG = a region where",
             "our replay is clearly darker (missing?) or brighter (extra?) than PCSX2; edge-only differences are ignored.",
             "Per capture: `pcsx2_cmp/<capture>/compare.png` (ours | PCSX2 | diff) and `grade.txt`.", "",
             "| capture | scene | verdict | mean diff | regions | flagged |", "|---|---|---|---|---|---|"]
    for r in results:
        scene = gsd.scene_label_at(scenes, r["tick"]) if scenes else "unknown"
        mean = f"{r['mean']:.2f}" if r["mean"] is not None else "-"
        lines.append(f"| {r['stem']} | {scene} | {r['verdict']} | {mean} | {len(r['regions'])} | {len(r['flagged'])} |")
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
