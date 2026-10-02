#!/usr/bin/env python3
"""One command: PCSX2 GS dump -> where does our rasterizer differ, and which draw is to blame.

    python gfx_scene_diff.py "<dump>.gs.zst"            # ref = sibling .png PCSX2 saved
    python gfx_scene_diff.py dump.gs.zst --ref other.png
    python gfx_scene_diff.py dump.gs.zst --rect 0,320,400,128   # also blame this region
    python gfx_scene_diff.py dump.gs.zst --selftest-skip 1234   # seeded-fault control

Pipeline (no game run, seconds):
  1. gsdump_parse.py -> .gsr (GIF stream) + .vram (the dump's starting VRAM)
  2. ps2x_gs_bench.exe replays it through OUR rasterizer -> our frame (BMP)
  3. compare to PCSX2's own frame (the screenshot it saves next to the dump)
  4. differing regions -> bench replay with PS2X_GSBENCH_WATCH lists every draw that
     overlaps the region and whether it changed a pixel. That names the draw.
Verdict per region:
  NO DRAW   no draw in the dump reaches the region (bad oracle frame or wrong fbp)
  NO EFFECT draws reach it but none changed a pixel (rejected: test/alpha/scissor/clip)
  OVERWRITTEN  last pixel-changing draw is listed (check what drew after the one you expect)
Needs ps2x_gs_bench.exe built from the current gs_bench_main.cpp (STOP/SKIP/WATCH options).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(r"F:\SDBZ Recomp")
BENCH = ROOT / "build" / "ps2xTest" / "RelWithDebInfo" / "ps2x_gs_bench.exe"
PARSE = ROOT / "build_scripts" / "gsdump_parse.py"
W, H = 512, 448
BLOCK = 16
WATCH_RE = re.compile(
    r"\[watch\] t=(\d+) changed=(\d) nonblack=(\d+) hash=(\w+)(?: draw bbox=\(([-\d.]+),([-\d.]+)\)-\(([-\d.]+),([-\d.]+)\) "
    r"prim=(\d+) tme=(\d) abe=(\d) fbp=(0x[0-9a-f]+) tbp0=(\d+) cbp=(\d+) tpsm=(\d+) ate=(\d) atst=(\d) zte=(\d) ztst=(\d) verts=(\d+)|( nodraw))")


def run(cmd, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, env=e, errors="replace")
    return p.returncode, p.stdout + p.stderr


def prepare(dump, out):
    gsr, vram = out / "frame.gsr", out / "frame.vram"
    if not gsr.exists() or gsr.stat().st_mtime < Path(dump).stat().st_mtime:
        rc, txt = run([sys.executable, str(PARSE), str(dump), "--emit-replay", str(gsr), "--init-state", "--emit-vram", str(vram)])
        if rc != 0 or not gsr.exists():
            sys.exit(f"gsdump_parse failed:\n{txt[-800:]}")
    return gsr


def display_geometry(dump, out):
    """Read DISPFB/DISPLAY/PMODE from the dump's privileged registers -> (fbp, fbw, w, h)."""
    import struct
    regs = out / "regs.bin"
    rc, txt = run([sys.executable, str(PARSE), str(dump), "--dump-regs", str(regs)])
    if rc != 0 or not regs.exists():
        return None
    r = regs.read_bytes()
    q = lambda off: struct.unpack_from("<Q", r, off)[0]
    pmode = q(0x00)
    use2 = bool(pmode & 2) and not (pmode & 1)
    dispfb = q(0x90 if use2 else 0x70)
    display = q(0xA0 if use2 else 0x80)
    fbp, fbw = dispfb & 0x1FF, (dispfb >> 9) & 0x3F
    magh, magv = (display >> 23) & 0xF, (display >> 27) & 3
    dw, dh = ((display >> 32) & 0xFFF) + 1, ((display >> 44) & 0x7FF) + 1
    w, h = dw // (magh + 1), dh // (magv + 1)
    if fbw == 0 or w <= 0 or h <= 0:
        return None
    return fbp, fbw, w, h


def render(gsr, bmp, fbp, fbw, extra=None):
    env = {"PS2X_GSBENCH_BMP": f"{fbp},{fbw},{W},{H},{bmp}", "PS2X_GS_RASTER_THREADS": "0"}
    if extra:
        env.update(extra)
    rc, txt = run([str(BENCH), str(gsr), "1"], env)
    if not Path(bmp).exists():
        sys.exit(f"bench produced no frame (rc={rc}):\n{txt[-800:]}")
    return txt


def load(path):
    return Image.open(path).convert("RGB")


def find_regions(ours, ref, thresh=24.0):
    """Block-wise mean abs diff -> connected regions of differing blocks."""
    d = np.abs(ours.astype(np.int16) - ref.astype(np.int16)).mean(axis=2)
    gh, gw = H // BLOCK, W // BLOCK
    blocks = d[: gh * BLOCK, : gw * BLOCK].reshape(gh, BLOCK, gw, BLOCK).mean(axis=(1, 3))
    flag = blocks > thresh
    seen = np.zeros_like(flag)
    regions = []
    for gy in range(gh):
        for gx in range(gw):
            if not flag[gy, gx] or seen[gy, gx]:
                continue
            stack, cells = [(gy, gx)], []
            seen[gy, gx] = True
            while stack:
                cy, cx = stack.pop()
                cells.append((cy, cx))
                for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                    if 0 <= ny < gh and 0 <= nx < gw and flag[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            ys = [c[0] for c in cells]
            xs = [c[1] for c in cells]
            x0, y0, x1, y1 = min(xs) * BLOCK, min(ys) * BLOCK, (max(xs) + 1) * BLOCK, (max(ys) + 1) * BLOCK
            sub_o, sub_r = ours[y0:y1, x0:x1], ref[y0:y1, x0:x1]
            regions.append({
                "rect": (x0, y0, x1 - x0, y1 - y0),
                "blocks": len(cells),
                "diff": float(d[y0:y1, x0:x1].mean()),
                "ours_lum": float(sub_o.mean()),
                "ref_lum": float(sub_r.mean()),
            })
    regions.sort(key=lambda r: -r["blocks"])
    return regions, d


def blame(gsr, rect, fbp, fbw, skip=None):
    x, y, w, h = rect
    env = {"PS2X_GSBENCH_WATCH": f"{fbp},{fbw},{x},{y},{w},{h}", "PS2X_GS_RASTER_THREADS": "0"}
    if skip is not None:
        env["PS2X_GSBENCH_SKIP"] = str(skip)
    rc, txt = run([str(BENCH), str(gsr), "1"], env)
    if "[gsbench] watch" not in txt:
        sys.exit("ps2x_gs_bench.exe is stale (no WATCH support): rebuild with build_scripts\\gs_bench.ps1 -Build")
    rows = []
    for line in txt.splitlines():
        m = WATCH_RE.match(line.strip())
        if not m:
            continue
        g = m.groups()
        if g[-1]:
            rows.append({"t": int(g[0]), "changed": g[1] == "1", "nodraw": True})
        else:
            rows.append({
                "t": int(g[0]), "changed": g[1] == "1", "nonblack": int(g[2]),
                "bbox": tuple(float(v) for v in g[4:8]), "prim": int(g[8]), "tme": int(g[9]), "abe": int(g[10]),
                "fbp": g[11], "tbp0": int(g[12]), "cbp": int(g[13]), "psm": int(g[14]),
                "ate": int(g[15]), "atst": int(g[16]), "zte": int(g[17]), "ztst": int(g[18]), "verts": int(g[19]),
            })
    return rows, txt


def describe(r):
    if r.get("nodraw"):
        return f"t={r['t']} CHANGED by a non-draw (image transfer/clear)"
    b = r["bbox"]
    return (f"t={r['t']} {'CHANGED' if r['changed'] else 'no-effect'} bbox=({b[0]:.1f},{b[1]:.1f})-({b[2]:.1f},{b[3]:.1f}) "
            f"prim={r['prim']} tme={r['tme']} abe={r['abe']} fbp={r['fbp']} tbp0={r['tbp0']} cbp={r['cbp']} psm={r['psm']} "
            f"ate={r['ate']} atst={r['atst']} zte={r['zte']} ztst={r['ztst']}")


def verdict(rows):
    draws = [r for r in rows if not r.get("nodraw")]
    if not draws:
        return "NO DRAW (nothing in the dump reaches this region)"
    changers = [r for r in draws if r["changed"]]
    if not changers:
        return "NO EFFECT (draws reach the region but none changed a pixel: alpha/z/scissor/clip reject)"
    return f"last pixel-changing draw: {describe(changers[-1])}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--ref", help="PCSX2 frame image (default: sibling .png of the dump)")
    ap.add_argument("--out", help="output dir (default: gsdump/scene_<dumpname>)")
    ap.add_argument("--fbp", type=lambda s: int(s, 0), help="presented frame buffer (default: from the dump's DISPFB)")
    ap.add_argument("--fbw", type=int, help="buffer width in 64px units (default: from DISPFB)")
    ap.add_argument("--size", help="WxH of the presented frame (default: from DISPLAY)")
    ap.add_argument("--rect", help="x,y,w,h extra region to blame (screen px)")
    ap.add_argument("--thresh", type=float, default=24.0, help="block mean-abs-diff to flag (default 24)")
    ap.add_argument("--top", type=int, default=4, help="regions to blame (default 4)")
    ap.add_argument("--selftest-skip", type=int, metavar="T",
                    help="control: skip transfer T in OUR replay, ref = full replay; the tool must blame T")
    a = ap.parse_args()

    if not BENCH.exists():
        sys.exit(f"missing {BENCH} (build: build_scripts\\gs_bench.ps1 -Build)")
    dump = Path(a.dump)
    out = Path(a.out) if a.out else ROOT / "gsdump" / ("scene_" + re.sub(r"\W+", "_", dump.name.split(".gs")[0]))
    out.mkdir(parents=True, exist_ok=True)

    gsr = prepare(dump, out)
    global W, H
    geo = display_geometry(dump, out)
    if geo:
        gfbp, gfbw, W, H = geo
        print(f"    display from dump regs: fbp=0x{gfbp:x} fbw={gfbw} {W}x{H}")
    else:
        gfbp, gfbw = 0x70, 8
        print("    WARNING no display regs; assuming fbp=0x70 fbw=8 512x448")
    if a.fbp is None:
        a.fbp = gfbp
    if a.fbw is None:
        a.fbw = gfbw
    if a.size:
        W, H = (int(v) for v in a.size.lower().split("x"))
    ours_bmp = out / "ours.bmp"
    print(f"[1] replay {dump.name}")
    skip = a.selftest_skip
    render(gsr, ours_bmp, a.fbp, a.fbw, {"PS2X_GSBENCH_SKIP": str(skip)} if skip is not None else None)

    if skip is not None:
        ref_bmp = out / "full.bmp"
        render(gsr, ref_bmp, a.fbp, a.fbw)
        ref_img = load(ref_bmp)
        ref_note = "ref = full replay (self-test)"
    else:
        ref_path = Path(a.ref) if a.ref else dump.with_name(dump.name.split(".gs")[0] + ".png")
        if not ref_path.exists():
            sys.exit(f"no reference image at {ref_path}; pass --ref")
        ref_img = load(ref_path)
        ref_note = f"ref = {ref_path.name} {ref_img.size}"
        if ref_img.size != (W, H):
            print(f"    WARNING ref is {ref_img.size}, not {W}x{H}: resized; set PCSX2 to native 1x resolution for exact diffs")
            ref_img = ref_img.resize((W, H), Image.LANCZOS)
    ours = np.asarray(load(ours_bmp))
    ref = np.asarray(ref_img)
    print(f"[2] {ref_note}")

    regions, d = find_regions(ours, ref, a.thresh)
    tot = float(d.mean())
    print(f"[3] mean abs diff {tot:.2f}; {len(regions)} differing region(s) (block {BLOCK}px, thresh {a.thresh})")
    if not regions:
        print("    no regions differ: our rasterizer reproduces PCSX2's frame for this dump.")

    todo = [(f"region#{i}", r["rect"], r) for i, r in enumerate(regions[: a.top])]
    if a.rect:
        rx = tuple(int(v) for v in a.rect.split(","))
        todo.append(("--rect", rx, None))
    for name, rect, r in todo:
        x, y, w, h = rect
        head = f"{name} ({x},{y}) {w}x{h}"
        if r:
            who = "OURS DARKER (missing?)" if r["ours_lum"] < r["ref_lum"] - 5 else (
                "ours brighter (extra?)" if r["ours_lum"] > r["ref_lum"] + 5 else "different content")
            head += f"  diff={r['diff']:.0f} lum ours={r['ours_lum']:.0f} ref={r['ref_lum']:.0f} -> {who}"
        print("\n" + head)
        rows, _ = blame(gsr, rect, a.fbp, a.fbw)
        print(f"    {verdict(rows)}")
        draws = [q for q in rows if not q.get("nodraw")]
        for q in draws[-6:]:
            print("      " + describe(q))
        if skip is not None and any(q["t"] == skip for q in rows):
            print(f"    self-test: transfer {skip} appears in the blame list -> PASS")

    # side-by-side image
    gap = np.full((H, 4, 3), 255, np.uint8)
    heat = np.clip(d * 3, 0, 255).astype(np.uint8)
    sheet = np.concatenate([ours, gap, ref, gap, np.stack([heat] * 3, axis=2)], axis=1)
    sheet_path = out / "compare.png"
    Image.fromarray(sheet).save(sheet_path)
    print(f"\n[4] ours | ref | diff-heat -> {sheet_path}")


if __name__ == "__main__":
    main()
