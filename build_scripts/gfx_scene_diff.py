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


def run(cmd, env=None, timeout=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=e, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as t:
        return -1, f"TIMEOUT after {timeout}s: {t.stdout or ''}{t.stderr or ''}"
    return p.returncode, p.stdout + p.stderr


def prepare(dump, out):
    gsr, vram = out / "frame.gsr", out / "frame.vram"
    if not gsr.exists() or gsr.stat().st_mtime < Path(dump).stat().st_mtime:
        rc, txt = run([sys.executable, str(PARSE), str(dump), "--emit-replay", str(gsr), "--init-state", "--emit-vram", str(vram)])
        if rc != 0 or not gsr.exists():
            sys.exit(f"gsdump_parse failed:\n{txt[-800:]}")
    return gsr


def gsr_regs(gsr):
    """Privileged register block stored in a .gsr (PCSX2 layout)."""
    import struct
    raw = Path(gsr).read_bytes()
    if raw[:4] != b"GSR1":
        sys.exit(f"{gsr} is not a GSR1 file")
    regs_size = struct.unpack_from("<I", raw, 12)[0]
    return raw[24:24 + regs_size]


def display_geometry(dump, out, regs_bytes=None):
    """Read DISPFB/DISPLAY/PMODE from the dump's privileged registers -> (fbp, fbw, w, h)."""
    import struct
    if regs_bytes is not None:
        r = regs_bytes
    else:
        regs = out / "regs.bin"
        rc, txt = run([sys.executable, str(PARSE), str(dump), "--dump-regs", str(regs)])
        if rc != 0 or not regs.exists():
            return None
        r = regs.read_bytes()
    if len(r) < 0xA8:
        return None
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
    # The bench hands fbp straight to GS::ReadVram, whose base is in BLOCKS; FRAME/DISPFB fbp is in pages (32 blocks).
    env = {"PS2X_GSBENCH_BMP": f"{fbp * 32},{fbw},{W},{H},{bmp}", "PS2X_GS_RASTER_THREADS": "0"}
    if extra:
        env.update(extra)
    rc, txt = run([str(BENCH), str(gsr), "1"], env)
    if not Path(bmp).exists():
        sys.exit(f"bench produced no frame (rc={rc}):\n{txt[-800:]}")
    return txt


def load(path):
    return Image.open(path).convert("RGB")


def pixel_diff(ours, ref, shift=0):
    """Per-pixel mean abs diff. shift=1: best match within +-1 px of ref, so the same art drawn
    a pixel off (edges only) does not count; missing or different art still does."""
    o = ours.astype(np.int16)
    r = ref.astype(np.int16)
    if not shift:
        return np.abs(o - r).mean(axis=2)
    p = np.pad(r, ((shift, shift), (shift, shift), (0, 0)), mode="edge")
    h, w = o.shape[:2]
    best = None
    for dy in range(2 * shift + 1):
        for dx in range(2 * shift + 1):
            d = np.abs(o - p[dy:dy + h, dx:dx + w]).mean(axis=2)
            best = d if best is None else np.minimum(best, d)
    return best


def find_regions(ours, ref, thresh=24.0, shift=0):
    """Block-wise mean abs diff -> connected regions of differing blocks."""
    d = pixel_diff(ours, ref, shift)
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
    env = {"PS2X_GSBENCH_WATCH": f"{fbp * 32},{fbw},{x},{y},{w},{h}", "PS2X_GS_RASTER_THREADS": "0"}
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


def steps(gsr, rect, fbp, fbw, out, rows, limit):
    """One frame per transfer that changed the rect: replay up to and including it."""
    ts = sorted({r["t"] for r in rows if r["changed"]})
    if len(ts) > limit:
        print(f"    {len(ts)} changing transfers; keeping the last {limit} (raise --steps-max)")
        ts = ts[-limit:]
    x, y, w, h = rect
    sdir = out / "steps"
    sdir.mkdir(exist_ok=True)
    for old in sdir.glob("step_*.png"):
        old.unlink()
    for t in ts:
        bmp = sdir / "step.bmp"
        if bmp.exists():
            bmp.unlink()
        render(gsr, bmp, fbp, fbw, {"PS2X_GSBENCH_STOP": str(t + 1)})
        img = load(bmp)
        img.save(sdir / f"step_{t:05d}.png")
        crop = np.asarray(img)[y:y + h, x:x + w]
        what = "; ".join(describe(r) for r in rows if r["t"] == t)[:230]
        print(f"      step t={t} rect lum={crop.mean():.1f} nonblack={int((crop.sum(axis=2) > 0).sum())}  {what}")
    print(f"    {len(ts)} step frame(s) -> {sdir}")


def last_complete_frame(gsr, fbw):
    """A capture stops mid-frame. The newest finished frame is the one sitting in a buffer
    just before that buffer's last full-screen clear -> (fbp_pages, stop_transfer, clears)."""
    rows, _ = blame(gsr, (0, 0, W, H), 0, fbw)
    clears = {}
    for r in rows:
        if r.get("nodraw") or r["tme"]:
            continue
        b = r["bbox"]
        if b[2] - b[0] >= W - 32 and b[3] - b[1] >= H - 48:
            clears.setdefault(int(r["fbp"], 16), []).append(r["t"])
    if not clears:
        return None
    fbp = max(clears, key=lambda k: clears[k][-1])
    return fbp, clears[fbp][-1], len(clears[fbp])


def tour(folder, out):
    """Replay every PS2X_GSCAP_EVERY capture in a folder -> one frame each + a contact sheet."""
    global W, H
    caps = sorted(folder.glob("*.gsr"), key=lambda p: int(re.search(r"_t(\d+)$", p.stem).group(1)) if re.search(r"_t(\d+)$", p.stem) else 0)
    if not caps:
        sys.exit(f"no .gsr captures in {folder}")
    out.mkdir(parents=True, exist_ok=True)
    frames, prev = [], None
    for gsr in caps:
        geo = display_geometry(gsr, out, gsr_regs(gsr))
        dfbp, fbw = (geo[0], geo[1]) if geo else (0x70, 8)
        W, H = (geo[2], geo[3]) if geo else (512, 448)
        lcf = last_complete_frame(gsr, fbw)
        fbp, stop, n = lcf if lcf else (dfbp, None, 0)
        bmp = out / "tour.bmp"
        if bmp.exists():
            bmp.unlink()
        render(gsr, bmp, fbp, fbw, {"PS2X_GSBENCH_STOP": str(stop)} if stop is not None else None)
        img = load(bmp)
        png = out / (gsr.stem + ".png")
        img.save(png)
        same = prev is not None and img.size == prev.size and not np.any(np.asarray(img) != np.asarray(prev))
        print(f"  {gsr.stem}: fbp=0x{fbp:x} stop={stop} clears={n} {W}x{H}{'  (same as previous)' if same else ''}")
        if not same:
            frames.append((gsr.stem, img))
        prev = img
    from PIL import ImageDraw
    cols = 4
    tw, th = 256, 224
    sheet = Image.new("RGB", (cols * tw, ((len(frames) + cols - 1) // cols) * (th + 14)), (32, 32, 32))
    dr = ImageDraw.Draw(sheet)
    for i, (name, img) in enumerate(frames):
        x, y = (i % cols) * tw, (i // cols) * (th + 14)
        sheet.paste(img.resize((tw, th), Image.LANCZOS), (x, y + 14))
        dr.text((x + 3, y + 1), name, fill=(255, 255, 0))
    sheet_path = out / "sheet.png"
    sheet.save(sheet_path)
    print(f"{len(caps)} capture(s), {len(frames)} distinct frame(s) -> {sheet_path}")


# ---- oracle: PCSX2 GSRunner renders the same capture; no human judges any frame ----

ORACLE_SHIFT = 1  # px of position slack: a 1-px offset is its own (reported) class, not missing art
# The oracle grades the raster path the GAME runs: worker threads (ps2_gs_raster_mt.inl), the
# default. render() alone forces 0 = the single-thread copy, which the game never uses.
ORACLE_RASTER_THREADS = os.environ.get("PS2X_ORACLE_RASTER_THREADS", "4")

def find_gsrunner():
    p = os.environ.get("PS2X_GSRUNNER")
    if p:
        return Path(p)
    hits = sorted(Path(r"F:\PCSX2-src\bin").glob("pcsx2-gsrunner*.exe"))
    return hits[0] if hits else None


def gsr_transfers(gsr):
    import struct
    raw = Path(gsr).read_bytes()
    _, count, regs_size, payload_size, _ = struct.unpack_from("<5I", raw, 4)
    ia = 24 + regs_size
    pa = ia + count * 12
    idx = struct.unpack_from(f"<{count * 3}I", raw, ia)
    return [raw[pa + idx[3 * i]: pa + idx[3 * i] + idx[3 * i + 1]] for i in range(count)]


def oracle_frame(gsr, fbp, fbw, psm, stop, work, runner):
    """PCSX2 SW renderer's view of buffer fbp after transfers [0, stop) -> RGB array, or None."""
    gs = work / "oracle.gs"
    cmd = [sys.executable, str(PARSE), str(gsr), "--gsr-to-gs", str(gs), "--probe", f"{fbp},{fbw},{psm},{W},{H}"]
    if stop is not None:
        cmd += ["--stop", str(stop)]
    rc, txt = run(cmd)
    if rc != 0:
        print(f"    gsr->gs failed: {txt[-400:]}")
        return None
    rt = work / "rt"
    rt.mkdir(exist_ok=True)
    for old in rt.glob("*"):
        old.unlink()
    # The probe is a GS->host read of the buffer; `-dump tr` saves it as *_read_<SBP>_*.bmp.
    # -loop 1: the replayer loops forever unless told (DumpReplayLoopCount defaults to 0).
    # Absolute dumpdir: a relative one lands under Documents\PCSX2.
    rc, txt = run([str(runner), "-renderer", "sw", "-dump", "tr", "-dumpdir", str(rt.resolve()), "-loop", "1",
                   "-surfaceless", "-noshadercache", "--", str(gs.resolve())], {"PCSX2_NOCONSOLE": "1"}, timeout=600)
    hits = sorted(rt.glob(f"*_read_{fbp * 32:05x}_*.*"), key=lambda p: int(p.name.split("_", 1)[0]))  # .png in practice
    if not hits:
        print(f"    GSRunner wrote no read-back dump (rc={rc}): {txt[-400:]}")
        return None
    img = np.asarray(load(hits[-1]))
    if img.shape[0] < H or img.shape[1] < W:
        print(f"    GSRunner rt0 is {img.shape[1]}x{img.shape[0]}, expected {W}x{H}")
        return None
    return img[:H, :W].copy()


# GIF decode for the lint (PACKED / REGLIST / IMAGE carried across transfers like ps2_gs_gpu).
_PRIM_VERTS = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}


def lint(gsr):
    """Flag primitives that are wrong INPUT (PCSX2 would draw them wrong too): triangles with an
    edge > 1200 px that still reach the screen (same rule as the [runaway] probe), and vertices
    on the VU1 guard-band clamp (raw 0x4000 / 0xBFFF). -> list of finding dicts."""
    return lint_blobs(enumerate(gsr_transfers(gsr)))


def lint_broken(f):
    """zero (top-left/raw-0 vertex) is never normal; plain clamp fans and corner fans at
    off-screen right/bottom are guard-band clipper output (PCSX2 sends them too)."""
    return f.get("zero", False)


def lint_line(stem, f):
    b = f["bbox"]
    return (f"{stem} t={f['t']} prim={f['prim']} edge={f['edge']:.0f} clamp={int(f['clamp'])} corner={int(f['corner'])} "
            f"zero={int(f.get('zero', False))} tbp0={f['tbp0']:#x} bbox=({b[0]:.0f},{b[1]:.0f})-({b[2]:.0f},{b[3]:.0f})")


def lint_blobs(blobs, xyoffset=None):
    """lint() over any GIF packets: blobs = iterable of (t, bytes), e.g. a .vucap's XGKICK
    packets with t = run index. xyoffset = raw (OFX, OFY) used until a packet sets XYOFFSET."""
    import struct
    found = []
    o = xyoffset or (1792 * 16, 1824 * 16)
    prim, ofs, tex0 = 0, [tuple(o), tuple(o)], [0, 0]
    q = []
    pending = 0

    def setreg(r, v, t):
        nonlocal prim, q
        if r == 0x00:
            prim, q = v & 0x7FF, []
        elif r in (0x18, 0x19):
            ofs[r - 0x18] = (v & 0xFFFF, (v >> 32) & 0xFFFF)
        elif r in (0x06, 0x07):
            tex0[r - 0x06] = v
        elif r in (0x04, 0x05, 0x0C, 0x0D):
            vert(v & 0xFFFF, (v >> 16) & 0xFFFF, r in (0x04, 0x05), t)

    def vert(x, y, kick, t):
        nonlocal q
        q.append((x, y))
        kind = prim & 7
        if kind not in (3, 4, 5):
            q = q[-_PRIM_VERTS.get(kind, 1):]
            return
        if len(q) < 3:
            return
        tri = q[-3:] if kind != 5 else [q[0], q[-2], q[-1]]
        # list: start over; strip: keep the last two; fan: keep the hub and the last one
        q = [] if kind == 3 else (q[-2:] if kind == 4 else [q[0], q[-1]])
        if not kick:
            return
        check(tri, t)

    def check(tri, t):
        ctx = (prim >> 9) & 1
        ox, oy = ofs[ctx]
        pts = [((a - ox) / 16.0, (b - oy) / 16.0) for a, b in tri]
        edge = max(((pts[i][0] - pts[i - 1][0]) ** 2 + (pts[i][1] - pts[i - 1][1]) ** 2) ** 0.5 for i in range(3))
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        visible = max(xs) >= 0 and min(xs) < W and max(ys) >= 0 and min(ys) < H
        clamp = any(a in (0x4000, 0xBFFF) or b in (0x4000, 0xBFFF) for a, b in tri)
        # zero = a vertex at raw (0,0) or clamped top-left (both axes <= 0x4000): what the VU1
        # skinning program 0x0e6 outputs from NaN bone matrices (the Krillin strip, 10-03).
        # Never seen in PCSX2 output; always BROKEN.
        zero = any(a <= 0x4000 and b <= 0x4000 for a, b in tri)
        if visible and (edge > 1200 or clamp or zero):
            # corner = one vertex on the clamp in BOTH x and y. Plain clamped fans on screen are
            # normal VU1 clipper output (PCSX2's own fight5full.vucap kicks have ~8 per frame);
            # corner and edge>1200-without-clamp were never seen in PCSX2 output (10-03).
            corner = any(a in (0x4000, 0xBFFF) and b in (0x4000, 0xBFFF) for a, b in tri)
            found.append({"t": t, "prim": prim & 7, "edge": edge, "clamp": clamp, "corner": corner, "zero": zero,
                          "tbp0": tex0[ctx] & 0x3FFF, "bbox": (min(xs), min(ys), max(xs), max(ys)),
                          "raw": [tuple(v) for v in tri]})

    for t, blob in blobs:
        off, end = 0, len(blob)
        if pending:
            take = min(pending * 16, end)
            off += take
            pending -= take // 16
        while off + 16 <= end:
            lo, hi = struct.unpack_from("<QQ", blob, off)
            off += 16
            nloop, pre, flg = lo & 0x7FFF, (lo >> 46) & 1, (lo >> 58) & 3
            nreg = ((lo >> 60) & 0xF) or 16
            regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
            if pre and flg != 2:
                setreg(0x00, (lo >> 47) & 0x7FF, t)
            if flg == 0:
                for _ in range(nloop):
                    for r in regs:
                        if off + 16 > end:
                            break
                        a, b = struct.unpack_from("<QQ", blob, off)
                        off += 16
                        if r == 0xE:
                            setreg(b & 0xFF, a, t)
                        elif r == 0x00:
                            setreg(0x00, a & 0x7FF, t)
                        elif r in (0x4, 0x5):  # packed XYZF2/XYZ2: x lo16, y bits 32..47, ADC bit 111
                            adc = (b >> 47) & 1
                            vert(a & 0xFFFF, (a >> 32) & 0xFFFF, not adc, t)
                        elif r in (0x6, 0x7):
                            tex0[r - 0x6] = a
            elif flg == 1:
                n = nloop * nreg
                for i in range(n):
                    p = off + 8 * i
                    if p + 8 > end:
                        break
                    setreg(regs[i % nreg], struct.unpack_from("<Q", blob, p)[0], t)
                off += ((n + 1) // 2) * 16
            else:
                avail = (end - off) // 16
                if nloop > avail:
                    pending = nloop - avail
                    off = end
                else:
                    off += nloop * 16
    return found


def first_divergent(gsr, rect, rows, fbp, fbw, psm, work, runner, thresh, env, cache):
    """Bisect the cut over the draws that touch rect: the first draw after which our rect
    differs from PCSX2's -> that transfer index (or None). A full-screen overlay drawn later
    no longer hides the culprit. cache: cut -> rect diff, shared by a capture's regions."""
    x, y, w, h = rect
    ts = sorted({r["t"] for r in rows if not r.get("nodraw")})

    def diverged(cut):
        if cut not in cache:
            bmp = work / "ours_cut.bmp"
            if bmp.exists():
                bmp.unlink()
            render(gsr, bmp, fbp, fbw, dict(env, PS2X_GSBENCH_STOP=str(cut)))
            ours = np.asarray(load(bmp))
            ref = oracle_frame(gsr, fbp, fbw, psm, cut, work, runner)
            cache[cut] = None if ref is None else pixel_diff(ours, ref, ORACLE_SHIFT)
        d = cache[cut]
        return d is not None and float(d[y:y + h, x:x + w].mean()) > thresh / 2

    lo, hi = -1, len(ts) - 1           # ts[hi] diverges (the full cut did); find the first one
    if hi < 0 or not diverged(ts[hi] + 1):
        return None
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if diverged(ts[mid] + 1):
            hi = mid
        else:
            lo = mid
    return ts[hi]


def oracle(folder, out, runner, thresh, only="*", skip=None, dedupe=False):
    """Every capture in a tour dir: our replay vs PCSX2 GSRunner at the same cut -> report.md.
    skip = seeded fault: drop that transfer from OUR replay only; the report must blame it.
    dedupe = skip GSRunner when our frame equals the last graded one (mean diff < 0.5): a long
    sweep sits on the same screen for minutes. The stream lint still sees every capture."""
    global W, H
    caps = sorted(folder.glob(only + ".gsr"), key=lambda p: int(re.search(r"_t(\d+)$", p.stem).group(1)) if re.search(r"_t(\d+)$", p.stem) else 0)
    if not caps:
        sys.exit(f"no .gsr captures in {folder}")
    out.mkdir(parents=True, exist_ok=True)
    work = out / "work"
    work.mkdir(exist_ok=True)
    lines, groups, lint_rows, offset_caps = [], {}, [], []
    last = None  # (stem, frame) of the last capture GSRunner graded
    for gsr in caps:
        regs = gsr_regs(gsr)
        geo = display_geometry(gsr, out, regs)
        dfbp, fbw = (geo[0], geo[1]) if geo else (0x70, 8)
        W, H = (geo[2], geo[3]) if geo else (512, 448)
        import struct
        pmode = struct.unpack_from("<Q", regs, 0)[0]
        psm = (struct.unpack_from("<Q", regs, 0x90 if (pmode & 2 and not pmode & 1) else 0x70)[0] >> 15) & 0x1F
        lcf = last_complete_frame(gsr, fbw)
        fbp, stop, _ = lcf if lcf else (dfbp, None, 0)
        for f in lint(gsr):
            if stop is None or f["t"] < stop:
                lint_rows.append((gsr.stem, f))
        bmp = work / "ours.bmp"
        if bmp.exists():
            bmp.unlink()
        env = {"PS2X_GSBENCH_STOP": str(stop)} if stop is not None else {}
        env["PS2X_GS_RASTER_THREADS"] = ORACLE_RASTER_THREADS
        if skip is not None:
            env["PS2X_GSBENCH_SKIP"] = str(skip)
        render(gsr, bmp, fbp, fbw, env)
        ours = np.asarray(load(bmp))
        if dedupe and last is not None and last[1].shape == ours.shape \
                and float(np.abs(last[1].astype(np.int16) - ours).mean()) < 0.5:
            lines.append(f"| {gsr.stem} | SAME | our frame = {last[0]} (not re-graded) | |")
            print(f"  {gsr.stem}: SAME as {last[0]}")
            continue
        last = (gsr.stem, ours)
        ref = oracle_frame(gsr, fbp, fbw, psm, stop, work, runner)
        if ref is None:
            lines.append(f"| {gsr.stem} | ERROR | GSRunner gave no frame | |")
            continue
        regions, d = find_regions(ours, ref, thresh, ORACLE_SHIFT)
        # Same art drawn about a pixel off: differs exactly but matches within +-1 px. Counted, not blamed.
        raw = pixel_diff(ours, ref)
        offpx = float(((raw > thresh) & (d <= thresh)).mean() * 100)
        if offpx >= 0.5:
            offset_caps.append((gsr.stem, offpx))
        if not regions:
            lines.append(f"| {gsr.stem} | MATCH | mean diff {float(d.mean()):.2f} (exact {float(raw.mean()):.2f}, 1-px offset px {offpx:.1f}%) | |")
            print(f"  {gsr.stem}: MATCH")
            continue
        gap = np.full((H, 4, 3), 255, np.uint8)
        heat = np.clip(d * 3, 0, 255).astype(np.uint8)
        Image.fromarray(np.concatenate([ours, gap, ref, gap, np.stack([heat] * 3, axis=2)], axis=1)).save(out / f"{gsr.stem}.png")
        cache = {}
        benv = {"PS2X_GSBENCH_SKIP": str(skip)} if skip is not None else {}
        benv["PS2X_GS_RASTER_THREADS"] = ORACLE_RASTER_THREADS
        for r in regions[:4]:
            rows, _ = blame(gsr, r["rect"], fbp, fbw)
            rows = [q for q in rows if stop is None or q["t"] < stop]
            ft = first_divergent(gsr, r["rect"], rows, fbp, fbw, psm, work, runner, thresh, benv, cache)
            if skip is not None:
                ok = ft == skip
                print(f"    self-test: first divergent draw = {ft}, seeded {skip} -> {'PASS' if ok else 'FAIL'}")
            first = next((q for q in rows if q["t"] == ft and not q.get("nodraw")), None)
            key = (f"prim={first['prim']} tme={first['tme']} abe={first['abe']} psm={first['psm']} "
                   f"ate={first['ate']} atst={first['atst']}") if first else "no divergent draw found"
            who = "ours darker" if r["ours_lum"] < r["ref_lum"] - 5 else ("ours brighter" if r["ours_lum"] > r["ref_lum"] + 5 else "different")
            x, y, w, h = r["rect"]
            groups.setdefault(key, []).append((gsr.stem, first["tbp0"] if first else None))
            why = f"first divergent draw: {describe(first)}" if first else verdict(rows)
            lines.append(f"| {gsr.stem} | DIFF | ({x},{y}) {w}x{h} {who}, diff {r['diff']:.0f} | {why[:180]} |")
        print(f"  {gsr.stem}: DIFF {len(regions)} region(s)")
    rep = out / "report.md"
    with open(rep, "w", encoding="utf-8") as f:
        f.write(f"# Oracle report: {folder}\n\nOurs (ps2x_gs_bench) vs PCSX2 GSRunner (sw) at the last complete frame of each capture.\n\n")
        f.write("## Distinct suspects (grouped by blamed draw state)\n\n")
        for key, hits in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            caps_hit = sorted({s for s, _ in hits})
            tbps = sorted({t for _, t in hits if t is not None})
            f.write(f"- {key}: {len(hits)} region(s) in {len(caps_hit)} capture(s), first {caps_hit[0]}; "
                    f"tbp0 {', '.join(f'{t:#x}' for t in tbps[:12])}{' ...' if len(tbps) > 12 else ''}\n")
        f.write(f"\n## 1-pixel offset class (same art, edges differ; not blamed)\n\n"
                f"{len(offset_caps)} capture(s) with >=0.5% of pixels off by about a pixel: "
                + ", ".join(f"{s} {p:.1f}%" for s, p in offset_caps[:60]) + "\n")
        f.write("\n## Per capture\n\n| capture | result | region | blame |\n|---|---|---|---|\n")
        f.write("\n".join(lines) + "\n")
        broken = [(s_, x) for s_, x in lint_rows if lint_broken(x)]
        normal = [(s_, x) for s_, x in lint_rows if not lint_broken(x)]
        f.write(f"\n## Stream lint BROKEN (zero=1: vertex at raw (0,0)/top-left clamp; never in PCSX2 output)\n\n"
                f"{len(broken)} triangle(s) in {len({s_ for s_, _ in broken})} capture(s).\n\n")
        for stem, fnd in broken[:200]:
            f.write(f"- {lint_line(stem, fnd)}\n")
        f.write(f"\n## Stream lint normal (plain clamp / corner fans = guard-band clipper output; PCSX2 sends them too)\n\n"
                f"{len(normal)} triangle(s).\n\n")
        for stem, fnd in normal[:60]:
            f.write(f"- {lint_line(stem, fnd)}\n")
    print(f"{len(caps)} capture(s) -> {rep}")


SCENE_RE = re.compile(r"\[scene\] tick=(\d+) (app=\S+ vt=\S+ mode=\d+ p1=0x[0-9a-f]{2})")
TICK_RE = re.compile(r"_t(\d+)")


def _tick(name):
    m = TICK_RE.search(name)
    return int(m.group(1)) if m else -1


def audit(folder):
    """One report for a capture dir, no human: oracle grade (if run), BROKEN stream lint,
    host-NaN VU1 scan, scene label per capture, disc coverage = screens never reached.
    Writes <dir>/report.md; returns the number of BROKEN findings (lint + NaN + DIFF)."""
    folder = Path(folder)
    caps = sorted(folder.glob("*.gsr"), key=lambda p: _tick(p.name))
    vucaps = sorted(folder.glob("*.vucap"), key=lambda p: _tick(p.name))

    # Scene timeline from the run log (PS2X_PAD_SCRIPT runs log [scene] on every change).
    scenes = []
    log = folder / "run_log.txt"
    if log.exists():
        raw = log.read_bytes()
        # launch_recomp -Log tees through PowerShell, which writes UTF-16 LE with a BOM.
        enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
        for line in raw.decode(enc, errors="replace").splitlines():
            for m in SCENE_RE.finditer(line):  # other logs can share the line
                scenes.append((int(m.group(1)), m.group(2)))
    scenes.sort()

    def scene_at(t):
        lab = "unknown"
        for st, txt in scenes:
            if st > t:
                break
            lab = txt
        return lab

    # 1. oracle (only when gfx_tour -Oracle / --oracle already graded this dir)
    orep = folder / "oracle" / "report.md"
    diffs, nmatch, graded = [], 0, orep.exists()
    if graded:
        for line in orep.read_text(encoding="utf-8").splitlines():
            if line.startswith("| cap_") or line.startswith("| vu_"):
                cells = [c.strip() for c in line.strip("|").split("|")]
                if len(cells) >= 2 and cells[1] in ("MATCH", "SAME"):
                    nmatch += 1
                elif len(cells) >= 2 and cells[1] == "DIFF":
                    diffs.append(cells)

    # 2. stream lint
    broken, normal = [], 0
    for gsr in caps:
        for f in lint(gsr):
            if lint_broken(f):
                broken.append((gsr.stem, f))
            else:
                normal += 1

    # 3. host NaN in VU1 memory
    nan_rows = []
    if vucaps:
        sys.path.insert(0, str(Path(__file__).parent))
        import contextlib
        import io
        import vucap_memscan
        for vc in vucaps:
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                try:
                    n = vucap_memscan.x86nan(str(vc))
                except SystemExit as e:
                    print(f"  skipped: {e}")
                    n = 0
            if n:
                nan_rows.append((vc.stem, buf.getvalue().strip().splitlines()))

    # 4. disc coverage
    cov = []
    if caps or (folder / "coverage_hashes.json").exists():  # json survives gfx_tour's prune
        r = subprocess.run([sys.executable, str(ROOT / "build_scripts" / "audit_disc_textures.py"),
                            "--coverage", str(folder)], capture_output=True, text=True)
        out = r.stdout.splitlines()
        k = next((i for i, l in enumerate(out) if l.startswith("uploads in captures")), None)
        cov = out[k:] if k is not None else [f"coverage failed: {r.stderr.strip()[-300:]}"]

    nbroken = len(broken) + len(nan_rows) + len(diffs)
    rep = folder / "report.md"
    with open(str(rep) + ".tmp", "w", encoding="utf-8") as f:
        f.write(f"# Graphics audit: {folder.name}\n\n")
        f.write(f"- captures: {len(caps)} GS, {len(vucaps)} VU1; scenes logged: {len(scenes)}\n")
        f.write(f"- **BROKEN: {nbroken}** = lint zero-vertex {len(broken)} tri(s) in "
                f"{len({s_ for s_, _ in broken})} cap(s), host-NaN VU1 {len(nan_rows)} cap(s), "
                f"oracle DIFF {len(diffs)}\n")
        f.write(f"- oracle: " + (f"{nmatch} MATCH, {len(diffs)} DIFF (details: oracle/report.md)" if graded
                                 else "not graded (gfx_tour.ps1 -Oracle)") + "\n")
        f.write(f"- normal lint (guard-band clamp fans, PCSX2 sends them too): {normal}\n")

        f.write("\n## Scene timeline\n\n")
        if scenes:
            for st, txt in scenes:
                f.write(f"- t={st} {txt}\n")
        else:
            f.write("none (no [scene] lines: run without PS2X_PAD_SCRIPT, or older build)\n")

        f.write("\n## BROKEN: oracle DIFF (ours vs PCSX2 raster)\n\n")
        for c in diffs:
            f.write(f"- {c[0]} [{scene_at(_tick(c[0]))}] {' | '.join(c[2:])[:220]}\n")
        if not diffs:
            f.write("none\n" if graded else "not graded\n")

        f.write("\n## BROKEN: zero-vertex triangles (vertex at raw (0,0)/top-left; the Krillin class)\n\n")
        per = {}
        for stem, fnd in broken:
            per.setdefault(stem, []).append(fnd)
        for stem, fs in per.items():
            tb = sorted({x["tbp0"] for x in fs})
            f.write(f"- {stem} [{scene_at(_tick(stem))}] {len(fs)} tri(s), tbp0 "
                    f"{', '.join(f'{t:#x}' for t in tb[:8])}; first: {lint_line(stem, fs[0])}\n")
        if not per:
            f.write("none\n")

        f.write("\n## BROKEN: host NaN (0x7FC00000/0xFFC00000) in VU1 memory (R5900 never makes these)\n\n")
        for stem, lines in nan_rows:
            f.write(f"- {stem} [{scene_at(_tick(stem))}]: {lines[-1]}\n")
            for l in lines[:-1][:4]:
                f.write(f"  - {l.strip()}\n")
        if not nan_rows:
            f.write("none\n" if vucaps else "no VU1 captures in this dir\n")

        f.write("\n## Disc coverage (folders with pictures never uploaded = screens not reached)\n\n```\n")
        f.write("\n".join(cov[:60]) + "\n```\n")
    os.replace(str(rep) + ".tmp", rep)
    print(f"BROKEN={nbroken} (lint {len(broken)}, nan caps {len(nan_rows)}, diff {len(diffs)}) -> {rep}")
    return nbroken


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="PCSX2 .gs/.gs.zst dump, or a .gsr from a live PS2X_GSCAP capture")
    ap.add_argument("--steps", action="store_true",
                    help="with --rect: write one frame per transfer that changed the rect (out/steps)")
    ap.add_argument("--steps-max", type=int, default=40)
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
    ap.add_argument("--oracle", action="store_true",
                    help="with a tour dir: grade every capture against PCSX2 GSRunner (sw) -> <dir>/oracle/report.md")
    ap.add_argument("--dedupe", action="store_true", help="with --oracle: skip GSRunner for frames equal to the last graded one")
    ap.add_argument("--only", default="*", help="with --oracle: capture name glob, e.g. cap_t7095")
    ap.add_argument("--audit", action="store_true",
                    help="with a capture dir: oracle + BROKEN lint + host-NaN VU1 scan + scenes + coverage -> <dir>/report.md")
    ap.add_argument("--lint", action="store_true", help="with a tour dir or .gsr: list runaway/clamped triangles only (no GSRunner)")
    a = ap.parse_args()

    if a.audit:
        audit(a.dump)
        return
    if not BENCH.exists():
        sys.exit(f"missing {BENCH} (build: build_scripts\\gs_bench.ps1 -Build)")
    dump = Path(a.dump)
    if a.lint:
        for gsr in ([dump] if not dump.is_dir() else sorted(dump.glob("*.gsr"))):
            for f in lint(gsr):
                print(lint_line(gsr.stem, f))
        return
    if dump.is_dir() and a.oracle:
        runner = find_gsrunner()
        if not runner or not runner.exists():
            sys.exit("no PCSX2 GSRunner: build F:\\PCSX2-src pcsx2-gsrunner, or set PS2X_GSRUNNER=<exe>")
        oracle(dump, Path(a.out) if a.out else dump / "oracle", runner, a.thresh, a.only, a.selftest_skip, a.dedupe)
        return
    if dump.is_dir():
        tour(dump, Path(a.out) if a.out else dump / "frames")
        return
    out =Path(a.out) if a.out else ROOT / "gsdump" / ("scene_" + re.sub(r"\W+", "_", dump.name.split(".gs")[0]))
    out.mkdir(parents=True, exist_ok=True)

    live = dump.suffix.lower() == ".gsr"
    if live and not dump.with_suffix(".vram").exists():
        print(f"    WARNING no {dump.with_suffix('.vram').name} next to the capture: VRAM starts zeroed")
    gsr = dump if live else prepare(dump, out)
    global W, H
    geo = display_geometry(dump, out, gsr_regs(gsr) if live else None)
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
    elif live and not a.ref:
        # A live capture has no oracle frame: blame/steps only.
        ref_img = load(ours_bmp)
        ref_note = "no ref (live capture): pass --ref <png> to compare, --rect to blame"
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
    if not regions and not (live and not a.ref):
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
        if a.steps and r is None:
            fbps = sorted({q["fbp"] for q in draws})
            print(f"    draws over this rect target fbp {', '.join(fbps) or 'none'}; watching fbp=0x{a.fbp:x} (--fbp to change)")
            steps(gsr, rect, a.fbp, a.fbw, out, rows, a.steps_max)

    # side-by-side image
    gap = np.full((H, 4, 3), 255, np.uint8)
    heat = np.clip(d * 3, 0, 255).astype(np.uint8)
    sheet = np.concatenate([ours, gap, ref, gap, np.stack([heat] * 3, axis=2)], axis=1)
    sheet_path = out / "compare.png"
    Image.fromarray(sheet).save(sheet_path)
    print(f"\n[4] ours | ref | diff-heat -> {sheet_path}")


if __name__ == "__main__":
    main()
