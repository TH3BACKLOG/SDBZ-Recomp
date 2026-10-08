#!/usr/bin/env python3
"""Step 2 of the feature-not-scene plan: GS feature matrix, our rasterizer vs PCSX2 SW.

Each case is a tiny synthetic GIF stream (uploads through IMAGE transfers, draws through
A+D), written as a .gsr + zero .vram. Both sides replay the same bytes:
  ours   ps2x_gs_bench.exe            (gfx_scene_diff.render)
  oracle PCSX2 GSRunner -renderer sw  (gfx_scene_diff.oracle_frame, .gsr -> .gs read-back)
No game run, no build. Seconds per case.

    python gs_feature_matrix.py                  # every case
    python gs_feature_matrix.py --only fan       # cases whose name contains "fan"
    python gs_feature_matrix.py --list

Verdict per case (RGB of the 64x64 CT32 frame, alpha is not read back by the bench):
  MATCH  no pixel differs by more than 2
  NEAR   differences only up to 8 (rounding: worth a look for filters)
  DIFF   any pixel off by more than 8 (wrong or missing pixels)
Control: case "control_skip_draw" drops our draw transfer (PS2X_GSBENCH_SKIP) and MUST be DIFF.
Output: Logs/gsfeature/matrix/report.md + <case>_{ours,ref,diff}.png (x4).
Gate (verify_fix.ps1): --gate exits 1 when a case is worse than baseline.json
(save it with --save-baseline after a verified change).
"""
import argparse
import json
import os
import random
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import gfx_scene_diff as S  # noqa: E402

OUT = HERE.parent.parent / "Logs" / "gsfeature" / "matrix"
BASELINE = HERE / "baseline.json"   # tracked: the verdicts a GS change must not make worse
RANK = {"MATCH": 0, "NEAR": 1, "DIFF": 2, "EMPTY": 3, "NOREF": 3}
W = H = 64
FBP, ZBP = 0, 8               # pages; 64x64 CT32 = 2 pages
TBP, CBP = 0x400, 0x600       # blocks, far from frame and z
VRAM_SIZE = 4 * 1024 * 1024
REGS_SIZE = 8192

# GS register addresses
PRIM, RGBAQ, ST, UV, XYZ2 = 0x00, 0x01, 0x02, 0x03, 0x05
TEX0_1, CLAMP_1, TEX1_1, TEX2_1 = 0x06, 0x08, 0x14, 0x16
XYOFFSET_1, PRMODECONT, TEXCLUT, SCANMSK = 0x18, 0x1A, 0x1C, 0x22
TEXA, FOGCOL, TEXFLUSH, SCISSOR_1, ALPHA_1 = 0x3B, 0x3D, 0x3F, 0x40, 0x42
DTHE, COLCLAMP, TEST_1, PABE, FBA_1, FRAME_1, ZBUF_1 = 0x45, 0x46, 0x47, 0x49, 0x4A, 0x4C, 0x4E
BITBLTBUF, TRXPOS, TRXREG, TRXDIR = 0x50, 0x51, 0x52, 0x53

CT32, CT24, CT16, T8, T4, T8H, T4HL, T4HH = 0x00, 0x01, 0x02, 0x13, 0x14, 0x1B, 0x24, 0x2C
P_TRI, P_STRIP, P_FAN, P_SPRITE = 3, 4, 5, 6


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def ad_packet(pairs):
    """PACKED A+D GIF packet: [(reg, data), ...]."""
    tag = len(pairs) | (1 << 15) | (1 << 60)
    blob = struct.pack("<QQ", tag, 0xE)
    for reg, data in pairs:
        blob += struct.pack("<QQ", data & 0xFFFFFFFFFFFFFFFF, reg)
    return blob


def image_packet(data):
    data = bytes(data) + b"\0" * (-len(data) % 16)
    tag = (len(data) // 16) | (1 << 15) | (2 << 58)
    return struct.pack("<QQ", tag, 0) + data


def upload(dbp, dbw, dpsm, w, h, data):
    """Host->local transfer: two GIF packets (setup A+D, IMAGE)."""
    return [ad_packet([(BITBLTBUF, (dbp << 32) | (dbw << 48) | (dpsm << 56)),
                       (TRXPOS, 0), (TRXREG, w | (h << 32)), (TRXDIR, 0)]),
            image_packet(data)]


def tex0(psm, tw_log, th_log, tcc=1, tfx=0, cpsm=CT32, csm=0, csa=0, cld=1, tbw=1):
    return (TBP | (tbw << 14) | (psm << 20) | (tw_log << 26) | (th_log << 30) | (tcc << 34) | (tfx << 35)
            | (CBP << 37) | (cpsm << 51) | (csm << 55) | (csa << 56) | (cld << 61))


def base_regs():
    """Transfer 0 (register restore): a 64x64 CT32 frame, Z32 at page 8, PRMODECONT=1."""
    return [(PRMODECONT, 1), (COLCLAMP, 1), (DTHE, 0), (PABE, 0), (SCANMSK, 0), (TEXA, 0x80 << 32 | 0),
            (FRAME_1, FBP | (1 << 16) | (CT32 << 24)), (ZBUF_1, ZBP), (XYOFFSET_1, 0),
            (SCISSOR_1, 0 | (63 << 16) | (0 << 32) | (63 << 48)), (TEST_1, 0x30000), (ALPHA_1, 0),
            (FBA_1, 0), (CLAMP_1, 0), (TEX1_1, 0), (TEXCLUT, 0)]


def xy(x, y):
    return (int(round(x * 16)) & 0xFFFF) | ((int(round(y * 16)) & 0xFFFF) << 16)


def rgba(r, g, b, a, q=1.0):
    return r | (g << 8) | (b << 16) | (a << 24) | (f32(q) << 32)


# ---------------------------------------------------------------- textures

def rand_bytes(rng, n):
    return bytes(rng.randrange(256) for _ in range(n))


def clut_ct32(rng, n, alpha=None):
    out = bytearray()
    for _ in range(n):
        out += bytes((rng.randrange(256), rng.randrange(256), rng.randrange(256),
                      alpha if alpha is not None else rng.randrange(0, 0x81)))
    return out


def tex_t4(rng, w=16, h=16, alpha=None):
    """Random 4-bit texture + 16-entry CT32 CLUT (CSM1 T4 CLUT = 8x2)."""
    return upload(TBP, max(1, w // 64), T4, w, h, rand_bytes(rng, w * h // 2)) + \
        upload(CBP, 1, CT32, 8, 2, clut_ct32(rng, 16, alpha))


def tex_t8(rng, w=16, h=16, alpha=None):
    """Random 8-bit texture + 256-entry CT32 CLUT (CSM1 T8 CLUT = 16x16, swizzled order is the
    GS's business: both sides get the same bytes)."""
    return upload(TBP, max(1, w // 64), T8, w, h, rand_bytes(rng, w * h)) + \
        upload(CBP, 1, CT32, 16, 16, clut_ct32(rng, 256, alpha))


def tex_ct32(rng, w=16, h=16, alpha=None):
    return upload(TBP, max(1, w // 64), CT32, w, h, clut_ct32(rng, w * h, alpha))


# ---------------------------------------------------------------- cases

CASES = {}


def case(name, desc):
    def deco(fn):
        CASES[name] = (desc, fn)
        return fn
    return deco


def draw(prim, verts, extra=()):
    """verts: list of dicts with xy=(x,y), rgba=(r,g,b,a), st=(s,t,q) or uv=(u,v), z."""
    pairs = list(extra) + [(PRIM, prim)]
    for v in verts:
        r, g, b, a = v.get("rgba", (128, 128, 128, 128))
        if "st" in v:
            s, t, q = v["st"]
            pairs.append((RGBAQ, rgba(r, g, b, a, q)))
            pairs.append((ST, f32(s) | (f32(t) << 32)))
        else:
            pairs.append((RGBAQ, rgba(r, g, b, a)))
        if "uv" in v:
            u, vv = v["uv"]
            pairs.append((UV, (int(u * 16) & 0x3FFF) | ((int(vv * 16) & 0x3FFF) << 16)))
        if "f" in v:
            pairs.append((FOG, v["f"] << 56))
        pairs.append((XYZ2, xy(*v["xy"]) | (v.get("z", 0x1000) << 32)))
    return ad_packet(pairs)


def rnd_rgba(rng, a=None):
    return (rng.randrange(256), rng.randrange(256), rng.randrange(256), a if a is not None else rng.randrange(0x81))


@case("fan_quad_subpixel", "ps2x_tests fan quad (6.375..26.25 px), flat white")
def _(rng):
    v = [(102 / 16, 102 / 16), (420 / 16, 102 / 16), (420 / 16, 420 / 16), (102 / 16, 420 / 16)]
    return [draw(P_FAN, [{"xy": p, "rgba": (255, 255, 255, 128)} for p in v])]


@case("fan_ngon_gouraud", "8-vertex fan, random subpixel rim, gouraud (SDBZ: trifan tme1 is used)")
def _(rng):
    import math
    cx, cy = 31.3, 32.7
    vs = [{"xy": (cx, cy), "rgba": rnd_rgba(rng, 128)}]
    for i in range(9):
        a = i * 2 * math.pi / 8
        vs.append({"xy": (cx + 25 * math.cos(a) + rng.random(), cy + 25 * math.sin(a) + rng.random()),
                   "rgba": rnd_rgba(rng, 128)})
    return [draw(P_FAN | (1 << 3), vs)]


@case("strip_gouraud_subpixel", "tristrip ribbon, random subpixel verts, gouraud")
def _(rng):
    vs = []
    for i in range(10):
        vs.append({"xy": (2 + i * 6.7 + rng.random(), 8 + 40 * (i % 2) + rng.random()), "rgba": rnd_rgba(rng, 128)})
    return [draw(P_STRIP | (1 << 3), vs)]


def textured(rng, tex_fn, prim, filt, use_st, extra_regs=(), tw=4, alpha=None, uvscale=1.0, tfx=0, tcc=1, psm=None):
    ups = tex_fn(rng, 1 << tw, 1 << tw, alpha)
    p = {tex_t4: T4, tex_t8: T8, tex_ct32: CT32}[tex_fn] if psm is None else psm
    regs = [(TEX0_1, tex0(p, tw, tw, tcc=tcc, tfx=tfx)), (TEX1_1, (filt << 5) | (filt << 6)), (TEXFLUSH, 0)] + list(extra_regs)
    size = (1 << tw) * uvscale
    # a rotated quad as 2 triangles strip so the sampler sees fractional texel steps
    quad = [((5.3, 4.1), (0, 0)), ((58.6, 9.4), (size, 0)), ((3.2, 57.7), (0, size)), ((55.9, 60.2), (size, size))]
    vs = []
    for (x, y), (u, v) in quad:
        d = {"xy": (x, y), "rgba": (128, 128, 128, 128)}
        if use_st:
            q = 1.0 + 0.5 * rng.random()
            d["st"] = (u / (1 << tw) * q, v / (1 << tw) * q, q)
        else:
            d["uv"] = (u, v)
        vs.append(d)
    pr = (P_STRIP | (1 << 4) | ((0 if use_st else 1) << 8)) if prim == "strip" else prim
    return ups + [draw(pr, vs, regs)]


for _psm, _fn in (("t4", tex_t4), ("t8", tex_t8), ("ct32", tex_ct32)):
    for _filt, _fname in ((0, "nearest"), (1, "bilinear")):
        for _st in (True, False):
            def _mk(fn=_fn, filt=_filt, st=_st):
                return lambda rng: textured(rng, fn, "strip", filt, st, extra_regs=[(CLAMP_1, 0)])
            CASES[f"tex_{_psm}_{_fname}_{'stq' if _st else 'uv'}"] = (
                f"{_psm.upper()} CLUT/texture, {_fname}, {'STQ' if _st else 'UV'}, REPEAT", _mk())


def sprite(rng, tex_fn, filt, x0, y0, x1, y1, u0, v0, u1, v1, use_st=False, tw=4):
    """Textured SPRITE (census: SDBZ draws 2D/UI with sprites). UV in texels."""
    ups = tex_fn(rng, 1 << tw, 1 << tw, None)
    p = {tex_t4: T4, tex_t8: T8, tex_ct32: CT32}[tex_fn]
    regs = [(TEX0_1, tex0(p, tw, tw)), (TEX1_1, (filt << 5) | (filt << 6)), (TEXFLUSH, 0), (CLAMP_1, 0)]
    vs = []
    for (x, y), (u, v) in (((x0, y0), (u0, v0)), ((x1, y1), (u1, v1))):
        d = {"xy": (x, y), "rgba": (128, 128, 128, 128)}
        if use_st:
            d["st"] = (u / (1 << tw), v / (1 << tw), 1.0)
        else:
            d["uv"] = (u, v)
        vs.append(d)
    return ups + [draw(P_SPRITE | (1 << 4) | ((0 if use_st else 1) << 8), vs, regs)]


for _psm, _fn in (("t4", tex_t4), ("t8", tex_t8), ("ct32", tex_ct32)):
    for _filt, _fname in ((0, "nearest"), (1, "bilinear")):
        CASES[f"sprite_{_psm}_{_fname}_uv_scaled"] = (
            f"SPRITE {_psm.upper()} {_fname} UV, 16x16 texels -> ~55x57 px at subpixel corners",
            (lambda fn, f: lambda rng: sprite(rng, fn, f, 4.5, 3.25, 59.75, 60.5, 0, 0, 16, 16))(_fn, _filt))


@case("sprite_t8_nearest_uv_1to1", "SPRITE T8 nearest UV 1:1 (UI glyph blit), 16x16 at (8,8)")
def _(rng):
    return sprite(rng, tex_t8, 0, 8, 8, 24, 24, 0, 0, 16, 16)


@case("sprite_t8_bilinear_uv_halftexel", "SPRITE T8 bilinear UV 1:1 with +0.5 texel offset (common 2D idiom)")
def _(rng):
    return sprite(rng, tex_t8, 1, 8, 8, 40, 40, 0.5, 0.5, 32.5, 32.5)


@case("sprite_t8_bilinear_stq", "SPRITE T8 bilinear STQ (q=1), 16x16 -> ~55x57 px")
def _(rng):
    return sprite(rng, tex_t8, 1, 4.5, 3.25, 59.75, 60.5, 0, 0, 16, 16, use_st=True)


@case("tex_t8_bilinear_stq_clamp","T8 bilinear STQ, CLAMP, uv overshoot 1.3x (edge texels)")
def _(rng):
    return textured(rng, tex_t8, "strip", 1, True, extra_regs=[(CLAMP_1, 1 | (1 << 2))], uvscale=1.3)


@case("tex_t8_bilinear_stq_repeat_wrap", "T8 bilinear STQ, REPEAT, uv 2.5x (wrap seams)")
def _(rng):
    return textured(rng, tex_t8, "strip", 1, True, extra_regs=[(CLAMP_1, 0)], uvscale=2.5)


def blend_case(alpha_reg, test=0x30000, abe=True, colclamp=1):
    def fn(rng):
        bg = draw(P_SPRITE, [{"xy": (0, 0), "rgba": (40, 90, 200, 0x40)}, {"xy": (64, 64), "rgba": (40, 90, 200, 0x40)}])
        vs = [{"xy": (4.5, 3.25), "rgba": rnd_rgba(rng)}, {"xy": (60.25, 10.5), "rgba": rnd_rgba(rng)},
              {"xy": (8.75, 60.5), "rgba": rnd_rgba(rng)}, {"xy": (59.5, 58.75), "rgba": rnd_rgba(rng)}]
        return [bg, draw(P_STRIP | (1 << 3) | ((1 if abe else 0) << 6), vs, [(ALPHA_1, alpha_reg), (TEST_1, test), (COLCLAMP, colclamp)])]
    return fn


# SDBZ blend equations (census): (Cs-Cd)*As+Cd, (Cs-Cs)*As+Cs, (Cs-0)*As+Cd
CASES["blend_cs_cd_as_cd"] = ("ALPHA (Cs-Cd)*As+Cd (SDBZ main)", blend_case(0 | (1 << 2) | (0 << 4) | (1 << 6)))
CASES["blend_cs_cs_as_cs"] = ("ALPHA (Cs-Cs)*As+Cs", blend_case(0 | (0 << 2) | (0 << 4) | (0 << 6)))
CASES["blend_cs_0_as_cd"] = ("ALPHA (Cs-0)*As+Cd additive", blend_case(0 | (2 << 2) | (0 << 4) | (1 << 6)))
CASES["blend_fix"] = ("ALPHA (Cs-Cd)*FIX+Cd, FIX=0x60", blend_case(0 | (1 << 2) | (2 << 4) | (1 << 6) | (0x60 << 32)))
# COLCLAMP=0 wraps (& 0xFF); our raster used to DROP the blend result there (ps2x_tests PABE caught it)
CASES["blend_cs_0_as_cd_colclamp0"] = ("ALPHA (Cs-0)*As+Cd additive, COLCLAMP=0 (overflow wraps)",
                                      blend_case(0 | (2 << 2) | (0 << 4) | (1 << 6), colclamp=0))
CASES["blend_cs_cd_as_cd_colclamp0"] = ("ALPHA (Cs-Cd)*As+Cd, COLCLAMP=0", blend_case(0 | (1 << 2) | (0 << 4) | (1 << 6), colclamp=0))
# SDBZ alpha tests (census): GREATER 64 KEEP, NOTEQUAL 0 KEEP
CASES["atest_greater64_keep"] = ("ATE GREATER ref 64 AFAIL KEEP", blend_case(0 | (1 << 2) | (1 << 6), 1 | (6 << 1) | (64 << 4) | 0x30000))
CASES["atest_notequal0_keep"] = ("ATE NOTEQUAL ref 0 AFAIL KEEP", blend_case(0 | (1 << 2) | (1 << 6), 1 | (7 << 1) | (0 << 4) | 0x30000))
# failing ps2x_tests features (not seen in SDBZ captures so far)
CASES["atest_afail_fb_only"] = ("ATE GEQUAL 0x80 AFAIL FB_ONLY", blend_case(0, 1 | (5 << 1) | (0x80 << 4) | (1 << 12) | 0x30000, abe=False))
CASES["atest_afail_rgb_only"] = ("ATE GEQUAL 0x80 AFAIL RGB_ONLY", blend_case(0, 1 | (5 << 1) | (0x80 << 4) | (3 << 12) | 0x30000, abe=False))


@case("tex1_t4_2texel_tri_bilinear", "ps2x_tests 'TEX1 linear T4 STQ' draw: 2x1 T4 black|white, tri s 0..1 over 4 px")
def _(rng):
    # texel 0 -> CLUT 0 black, texel 1 -> CLUT 1 white (low nibble = texel 0)
    clut = bytes((0, 0, 0, 0x80, 255, 255, 255, 0x80)) + bytes(14 * 4)
    ups = upload(TBP, 1, T4, 2, 1, bytes((0x10,))) + upload(CBP, 1, CT32, 8, 2, clut)
    regs = [(TEX0_1, tex0(T4, 1, 0)), (TEX1_1, (1 << 5) | (1 << 6)), (TEXFLUSH, 0), (CLAMP_1, 0)]
    vs = [{"xy": (0, 0), "st": (0.0, 0.0, 1.0)}, {"xy": (4, 0), "st": (1.0, 0.0, 1.0)},
          {"xy": (0, 4), "st": (0.0, 0.0, 1.0)}]
    return ups + [draw(P_TRI | (1 << 4), vs, regs)]


# ---- census COMBOS (10-08c): single-feature cases hid the sprite + COLCLAMP bugs.
# Top real combos: tristrip iip1 T8 CSM1 bilinear STQ CLAMP MODULATE tcc1 (vertex
# colour != 128) + (Cs-Cd)*As+Cd + ATST GREATER 64 + ZTST GEQUAL Z32.
A_MAIN = 0 | (1 << 2) | (0 << 4) | (1 << 6)                    # (Cs-Cd)*As+Cd
T_FIGHTER = 1 | (6 << 1) | (64 << 4) | (1 << 16) | (2 << 17)   # ATST GREATER 64 KEEP, ZTST GEQUAL
T_HUD = 1 | (7 << 1) | (0 << 4) | (1 << 16) | (1 << 17)        # ATST NOTEQUAL 0 KEEP, ZTST ALWAYS
CLAMP_CLAMP = 1 | (1 << 2)


def combo_bg():
    return draw(P_SPRITE, [{"xy": (0, 0), "rgba": (40, 90, 200, 0x80), "z": 0},
                           {"xy": (64, 64), "rgba": (40, 90, 200, 0x80), "z": 0}],
                [(TEST_1, 0x30000), (ALPHA_1, 0)])


def combo_strip(rng, psm, filt, z0, z1, ys=(4.1, 59.2)):
    """Gouraud perspective tristrip ribbon, per-vertex z in z0..z1, uv overshoot -> CLAMP edges."""
    vs = []
    n = 8
    for i in range(n):
        x = 3.3 + 57.3 * i / (n - 1) + rng.random() * 0.9
        y = ys[i % 2] + rng.random() * 0.9
        q = 0.6 + rng.random()
        u, v = (i / (n - 1)) * 1.1, (i % 2) * 1.05
        vs.append({"xy": (x, y), "st": (u * q, v * q, q), "z": rng.randrange(z0, z1),
                   "rgba": (rng.randrange(256), rng.randrange(256), rng.randrange(256), 0x80)})
    regs = [(TEX0_1, tex0(psm, 4, 4)), (TEX1_1, (filt << 5) | (filt << 6)), (TEXFLUSH, 0),
            (CLAMP_1, CLAMP_CLAMP), (ALPHA_1, A_MAIN), (TEST_1, T_FIGHTER)]
    return draw(P_STRIP | (1 << 3) | (1 << 4) | (1 << 6), vs, regs)


@case("combo_fighter_t8_bilinear", "SDBZ top combo: 2 overlapping gouraud T8 bilinear STQ CLAMP strips, blend, ATST>64, ZTST GEQUAL")
def _(rng):
    return tex_t8(rng) + [combo_bg(), combo_strip(rng, T8, 1, 0x1000, 0x8000),
                          combo_strip(rng, T8, 1, 0x4000, 0xC000, ys=(30.5, 6.2))]


@case("combo_fighter_ct32_nearest", "SDBZ CT32 nearest combo: gouraud STQ CLAMP strips, blend, ATST>64, ZTST GEQUAL")
def _(rng):
    return tex_ct32(rng) + [combo_bg(), combo_strip(rng, CT32, 0, 0x1000, 0x8000),
                            combo_strip(rng, CT32, 0, 0x4000, 0xC000, ys=(30.5, 6.2))]


@case("combo_fan_t4_bilinear", "SDBZ trifan tme1: gouraud T4 bilinear STQ fan, blend, ATST>64, ZTST GEQUAL")
def _(rng):
    import math
    cx, cy = 31.4, 30.8
    vs = [{"xy": (cx, cy), "st": (0.5, 0.5, 1.0), "z": 0x6000, "rgba": (200, 180, 160, 0x80)}]
    for i in range(9):
        a = i * 2 * math.pi / 8
        q = 0.7 + rng.random() * 0.6
        vs.append({"xy": (cx + 27 * math.cos(a) + rng.random(), cy + 27 * math.sin(a) + rng.random()),
                   "st": ((0.5 + 0.55 * math.cos(a)) * q, (0.5 + 0.55 * math.sin(a)) * q, q),
                   "z": rng.randrange(0x1000, 0x9000), "rgba": rnd_rgba(rng, 0x80)})
    regs = [(TEX0_1, tex0(T4, 4, 4)), (TEX1_1, (1 << 5) | (1 << 6)), (TEXFLUSH, 0),
            (CLAMP_1, CLAMP_CLAMP), (ALPHA_1, A_MAIN), (TEST_1, T_FIGHTER)]
    return tex_t4(rng) + [combo_bg(), draw(P_FAN | (1 << 3) | (1 << 4) | (1 << 6), vs, regs)]


@case("combo_hud_sprite_stq", "SDBZ HUD: STQ sprites T8 nearest CLAMP, vertex colour != 128, blend, ATST NOTEQUAL 0")
def _(rng):
    out = tex_t8(rng) + [combo_bg()]
    regs = [(TEX0_1, tex0(T8, 4, 4)), (TEX1_1, 0), (TEXFLUSH, 0), (CLAMP_1, CLAMP_CLAMP),
            (ALPHA_1, A_MAIN), (TEST_1, T_HUD)]
    for x0, y0, x1, y1, s1, t1 in ((2.5, 3.0, 30.75, 20.25, 1.0, 1.0), (20.0, 15.5, 61.0, 50.0, 1.3, 0.8),
                                   (5.0, 40.0, 13.0, 48.0, 0.5, 0.5)):
        c = (rng.randrange(256), rng.randrange(256), rng.randrange(256), 0x80)
        out.append(draw(P_SPRITE | (1 << 4) | (1 << 6), [
            {"xy": (x0, y0), "st": (0.0, 0.0, 1.0), "rgba": c, "z": 0x100},
            {"xy": (x1, y1), "st": (s1, t1, 1.0), "rgba": c, "z": 0x100}], regs))
    return out


# ---- UNSEEN features (10-08c): not in the census of reached scenes, but unreached
# scenes (supers eff/, dis/ screens) may use them. A DIFF here = a likely missing/
# wrong graphic we have not reached yet. The grade is RGB only, so alpha-only
# features are made visible with blending / a second draw.
FOG = 0x0A
A_DST = 0 | (1 << 2) | (1 << 4) | (1 << 6)             # (Cs-Cd)*Ad+Cd: shows FRAME alpha
ABE = 1 << 6


def tex_ct24(rng, w=16, h=16, alpha=None):
    return upload(TBP, 1, CT24, w, h, rand_bytes(rng, w * h * 3))


def tex_ct16(rng, w=16, h=16, alpha=None):
    px = bytearray(rand_bytes(rng, w * h * 2))
    for i in range(0, len(px), 2 * 7):                  # some pure-black texels for AEM
        px[i] = px[i + 1] = 0
    return upload(TBP, 1, CT16, w, h, bytes(px))


def tstrip(rng, ups, t0, filt=0, st=True, regs=(), pflags=0, uvscale=1.0, gouraud=False, fog=False, tw=4):
    size = (1 << tw) * uvscale
    quad = [((5.3, 4.1), (0, 0)), ((58.6, 9.4), (size, 0)), ((3.2, 57.7), (0, size)), ((55.9, 60.2), (size, size))]
    vs = []
    for (x, y), (u, v) in quad:
        d = {"xy": (x, y), "rgba": rnd_rgba(rng, 0x80) if gouraud else (128, 128, 128, 128)}
        if st:
            q = 1.0 + 0.5 * rng.random()
            d["st"] = (u / (1 << tw) * q, v / (1 << tw) * q, q)
        else:
            d["uv"] = (u, v)
        if fog:
            d["f"] = rng.randrange(256)
        vs.append(d)
    r = [(TEX0_1, t0), (TEX1_1, (filt << 5) | (filt << 6)), (TEXFLUSH, 0)] + list(regs)
    pr = P_STRIP | (1 << 4) | ((0 if st else 1) << 8) | ((1 if gouraud else 0) << 3) | pflags
    return ups + [combo_bg(), draw(pr, vs, r)]


def unseen(name, desc, fn):
    CASES[name] = (desc, fn)


unseen("unseen_tex_ct24_texa_blend", "CT24 texture, TEXA TA0=0x30 -> alpha, blended (alpha visible)",
       lambda rng: tstrip(rng, tex_ct24(rng), tex0(CT24, 4, 4), regs=[(TEXA, 0x30 | (0x70 << 32)), (ALPHA_1, A_MAIN)], pflags=ABE))
unseen("unseen_tex_ct16_texa_aem_blend", "CT16 texture, TEXA AEM=1 TA0=0x20 TA1=0x70, black texels -> A=0, blended",
       lambda rng: tstrip(rng, tex_ct16(rng), tex0(CT16, 4, 4), regs=[(TEXA, 0x20 | (1 << 15) | (0x70 << 32)), (ALPHA_1, A_MAIN)], pflags=ABE))
unseen("unseen_tex_ct16_texa_noaem_bilinear", "CT16 texture, TEXA AEM=0, bilinear STQ, blended",
       lambda rng: tstrip(rng, tex_ct16(rng), tex0(CT16, 4, 4), filt=1, regs=[(TEXA, 0x10 | (0x60 << 32)), (ALPHA_1, A_MAIN)], pflags=ABE))
unseen("unseen_tfx_decal", "TFX DECAL tcc1 gouraud (vertex colour ignored)",
       lambda rng: tstrip(rng, tex_t8(rng), tex0(T8, 4, 4, tfx=1), gouraud=True))
unseen("unseen_tfx_highlight2_blend", "TFX HIGHLIGHT2 T8 gouraud, blended",
       lambda rng: tstrip(rng, tex_t8(rng), tex0(T8, 4, 4, tfx=3), gouraud=True, regs=[(ALPHA_1, A_MAIN)], pflags=ABE))
unseen("unseen_tfx_modulate_tcc0_blend", "TFX MODULATE TCC=0 (vertex alpha) gouraud T8 bilinear, blended",
       lambda rng: tstrip(rng, tex_t8(rng), tex0(T8, 4, 4, tcc=0), filt=1, gouraud=True, regs=[(ALPHA_1, A_MAIN)], pflags=ABE))
unseen("unseen_clamp_region_clamp_bilinear", "REGION_CLAMP u 2..12 v 3..11, T8 bilinear STQ, uv 1.3x",
       lambda rng: tstrip(rng, tex_t8(rng), tex0(T8, 4, 4), filt=1, uvscale=1.3,
                          regs=[(CLAMP_1, 2 | (2 << 2) | (2 << 4) | (12 << 14) | (3 << 24) | (11 << 34))]))
unseen("unseen_clamp_region_repeat_nearest", "REGION_REPEAT umsk 7 ufix 8, vmsk 3 vfix 4, T8 nearest STQ, uv 2x",
       lambda rng: tstrip(rng, tex_t8(rng), tex0(T8, 4, 4), uvscale=2.0,
                          regs=[(CLAMP_1, 3 | (3 << 2) | (7 << 4) | (8 << 14) | (3 << 24) | (4 << 34))]))
unseen("unseen_fog_gouraud_tex", "FGE fog per vertex, FOGCOL, gouraud T8 STQ",
       lambda rng: tstrip(rng, tex_t8(rng), tex0(T8, 4, 4), gouraud=True, fog=True,
                          regs=[(FOGCOL, 0x30 | (0xA0 << 8) | (0x50 << 16))], pflags=1 << 5))
unseen("unseen_tex_t8h", "T8H texture (index in CT32 bits 24-31), nearest STQ",
       lambda rng: tstrip(rng, upload(TBP, 1, T8H, 16, 16, rand_bytes(rng, 256)) + upload(CBP, 1, CT32, 16, 16, clut_ct32(rng, 256)),
                          tex0(T8H, 4, 4)))
unseen("unseen_tex_t4hl", "T4HL texture (index in CT32 bits 24-27), nearest STQ",
       lambda rng: tstrip(rng, upload(TBP, 1, T4HL, 16, 16, rand_bytes(rng, 128)) + upload(CBP, 1, CT32, 8, 2, clut_ct32(rng, 16)),
                          tex0(T4HL, 4, 4)))
unseen("unseen_tex_t4hh", "T4HH texture (index in CT32 bits 28-31), nearest STQ",
       lambda rng: tstrip(rng, upload(TBP, 1, T4HH, 16, 16, rand_bytes(rng, 128)) + upload(CBP, 1, CT32, 8, 2, clut_ct32(rng, 16)),
                          tex0(T4HH, 4, 4)))
unseen("unseen_clut_ct16_t8", "T8 texture with a CT16 CLUT (cpsm=CT16, CSM1)",
       lambda rng: tstrip(rng, upload(TBP, 1, T8, 16, 16, rand_bytes(rng, 256)) + upload(CBP, 1, CT16, 16, 16, rand_bytes(rng, 512)),
                          tex0(T8, 4, 4, cpsm=CT16)))
unseen("unseen_clut_csm2_t8", "T8 texture with a CSM2 CLUT (linear, TEXCLUT cbw=4 cou=0 cov=2)",
       lambda rng: tstrip(rng, upload(TBP, 1, T8, 16, 16, rand_bytes(rng, 256)) + upload(CBP, 4, CT32, 256, 4, clut_ct32(rng, 1024)),
                          tex0(T8, 4, 4, csm=1), regs=[(TEXCLUT, 4 | (0 << 6) | (2 << 12))]))


@case("unseen_fba_then_dst_alpha", "FBA=1 strip (alpha MSB forced), then a sprite blended by FRAME alpha (Ad)")
def _(rng):
    s = tstrip(rng, tex_t8(rng), tex0(T8, 4, 4), gouraud=True, regs=[(FBA_1, 1)])
    cover = draw(P_SPRITE | ABE, [{"xy": (0, 0), "rgba": (250, 20, 30, 0x80)}, {"xy": (64, 64), "rgba": (250, 20, 30, 0x80)}],
                 [(FBA_1, 0), (ALPHA_1, A_DST), (TEST_1, 0x30000)])
    return s + [cover]


@case("unseen_date_datm0", "DATE/DATM=0: gouraud strip writes alpha 0..0xFF, then a sprite drawn only where dest alpha MSB=0")
def _(rng):
    vs = []
    for i in range(10):
        vs.append({"xy": (2 + i * 6.7 + rng.random(), 4 + 52 * (i % 2) + rng.random()),
                   "rgba": (rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.choice((0, 0x40, 0xC0, 0xFF)))})
    under = draw(P_STRIP | (1 << 3), vs, [(TEST_1, 0x30000)])
    over = draw(P_SPRITE, [{"xy": (0, 0), "rgba": (255, 255, 0, 0x80)}, {"xy": (64, 64), "rgba": (255, 255, 0, 0x80)}],
                [(TEST_1, 0x30000 | (1 << 14) | (0 << 15))])
    return [combo_bg(), under, over]


@case("unseen_afail_zb_only", "ATST NEVER AFAIL=ZB_ONLY writes z only: a later GEQUAL sprite is hidden where z was written")
def _(rng):
    vs = [{"xy": (2 + i * 6.7, 4 + 52 * (i % 2)), "rgba": (255, 0, 0, 0x80), "z": 0xF000} for i in range(10)]
    zonly = draw(P_STRIP, vs, [(TEST_1, 1 | (0 << 1) | (2 << 12) | (1 << 16) | (1 << 17))])
    back = draw(P_SPRITE, [{"xy": (0, 10), "rgba": (0, 255, 0, 0x80), "z": 0x8000},
                           {"xy": (64, 50), "rgba": (0, 255, 0, 0x80), "z": 0x8000}],
                [(TEST_1, (1 << 16) | (2 << 17))])
    return [combo_bg(), zonly, back]


def zfmt_case(zpsm, zmax):
    def fn(rng):
        regs = [(ZBUF_1, ZBP | (zpsm << 24)), (TEST_1, (1 << 16) | (2 << 17))]
        a = [{"xy": (2 + i * 6.7 + rng.random(), 4 + 52 * (i % 2) + rng.random()), "rgba": rnd_rgba(rng, 0x80),
              "z": rng.randrange(zmax // 4, zmax)} for i in range(10)]
        b = [{"xy": (4 + 52 * (i % 2) + rng.random(), 2 + i * 6.7 + rng.random()), "rgba": rnd_rgba(rng, 0x80),
              "z": rng.randrange(zmax // 4, zmax)} for i in range(10)]
        clear = draw(P_SPRITE, [{"xy": (0, 0), "rgba": (0, 0, 0, 0), "z": 0}, {"xy": (64, 64), "rgba": (0, 0, 0, 0), "z": 0}],
                     [(ZBUF_1, ZBP | (zpsm << 24)), (TEST_1, 0x30000)])
        return [clear, draw(P_STRIP | (1 << 3), a, regs), draw(P_STRIP | (1 << 3), b, regs)]
    return fn


CASES["unseen_z24_gequal"] = ("Z24 buffer: two crossing gouraud strips, ZTST GEQUAL", zfmt_case(1, 1 << 24))
CASES["unseen_z16_gequal"] = ("Z16 buffer: two crossing gouraud strips, ZTST GEQUAL", zfmt_case(2, 1 << 16))


@case("unseen_linestrip_gouraud", "LINESTRIP gouraud zig-zag (no lines in the census)")
def _(rng):
    vs = [{"xy": (2 + i * 4.3 + rng.random(), 5 + 50 * (i % 2) + rng.random()), "rgba": rnd_rgba(rng, 0x80)} for i in range(14)]
    return [combo_bg(), draw(2 | (1 << 3), vs, [(TEST_1, 0x30000)])]


@case("unseen_scanmsk_even", "SCANMSK=2 (even lines masked) gouraud strip")
def _(rng):
    vs = [{"xy": (2 + i * 6.7 + rng.random(), 4 + 52 * (i % 2) + rng.random()), "rgba": rnd_rgba(rng, 0x80)} for i in range(10)]
    return [combo_bg(), draw(P_STRIP | (1 << 3), vs, [(SCANMSK, 2), (TEST_1, 0x30000)])]


@case("unseen_tex2_clut_switch", "TEX2 switches CBP to a second CLUT (cld=1) without a TEX0 write; 2 strips")
def _(rng):
    CBP2 = CBP + 0x40
    ups = upload(TBP, 1, T8, 16, 16, rand_bytes(rng, 256)) + upload(CBP, 1, CT32, 16, 16, clut_ct32(rng, 256)) +         upload(CBP2, 1, CT32, 16, 16, clut_ct32(rng, 256))
    t0 = tex0(T8, 4, 4)
    tex2 = (T8 << 20) | (CBP2 << 37) | (CT32 << 51) | (1 << 61)
    a = tstrip(rng, [], t0)
    vs = [{"xy": (2.3 + i * 6.7, 34.3 + 25.6 * (i % 2)), "st": ((i // 2) / 4.1, (i % 2) * 0.97, 1.0), "rgba": (128, 128, 128, 128)}
          for i in range(10)]
    b = draw(P_STRIP | (1 << 4), vs, [(TEX2_1, tex2), (TEXFLUSH, 0)])
    return ups + a + [b]


@case("ztest_gequal","two overlapping strips, ZTST GEQUAL, near one drawn first")
def _(rng):
    a = [{"xy": (4, 4), "rgba": (255, 0, 0, 128), "z": 0x8000}, {"xy": (50, 6), "rgba": (255, 0, 0, 128), "z": 0x8000},
         {"xy": (6, 50), "rgba": (255, 0, 0, 128), "z": 0x8000}]
    b = [{"xy": (14, 12), "rgba": (0, 255, 0, 128), "z": 0x100}, {"xy": (60, 30), "rgba": (0, 255, 0, 128), "z": 0xF000},
         {"xy": (20, 60), "rgba": (0, 255, 0, 128), "z": 0x100}]
    t = [(TEST_1, (1 << 16) | (2 << 17)), (ZBUF_1, ZBP)]
    return [draw(P_TRI, a, t), draw(P_TRI, b)]


@case("pabe", "PABE=1: low-alpha (<0x80) pixels skip blending")
def _(rng):
    return blend_case(0 | (1 << 2) | (1 << 6))(rng)[:1] + [draw(
        P_STRIP | (1 << 3) | (1 << 6),
        [{"xy": (4, 4), "rgba": (250, 10, 10, 0x20)}, {"xy": (60, 4), "rgba": (250, 10, 10, 0xC0)},
         {"xy": (4, 60), "rgba": (10, 250, 10, 0x20)}, {"xy": (60, 60), "rgba": (10, 250, 10, 0xC0)}],
        [(ALPHA_1, 0 | (1 << 2) | (1 << 6)), (PABE, 1)])]


@case("tfx_highlight", "TFX HIGHLIGHT T8 bilinear STQ")
def _(rng):
    return textured(rng, tex_t8, "strip", 1, True, tfx=2)


@case("tfx_modulate_tcc0", "TFX MODULATE TCC=0 T8 nearest UV")
def _(rng):
    return textured(rng, tex_t8, "strip", 0, False, tcc=0)


@case("control_skip_draw", "CONTROL: same as fan_ngon_gouraud, our side skips the draw -> must DIFF")
def _(rng):
    return CASES["fan_ngon_gouraud"][1](rng)


# ---------------------------------------------------------------- run

def write_gsr(path, packets):
    transfers = [ad_packet(base_regs())] + packets
    blob, index = bytearray(), bytearray()
    for t in transfers:
        index += struct.pack("<III", len(blob), len(t), 0)
        blob += t
    with open(path, "wb") as f:
        f.write(b"GSR1")
        f.write(struct.pack("<IIIII", 1, len(transfers), REGS_SIZE, len(blob), 0))
        f.write(bytes(REGS_SIZE))
        f.write(index)
        f.write(blob)
    path.with_suffix(".vram").write_bytes(bytes(VRAM_SIZE))
    return len(transfers)


def grade(ours, ref):
    d = np.abs(ours.astype(np.int16) - ref.astype(np.int16)).max(axis=2)
    mx = int(d.max())
    n2, n8 = int((d > 2).sum()), int((d > 8).sum())
    verdict = "MATCH" if n2 == 0 else ("NEAR" if n8 == 0 else "DIFF")
    return verdict, mx, n2, n8, d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--save-baseline", action="store_true", help=f"write {BASELINE.name} from this run")
    ap.add_argument("--gate", action="store_true", help="exit 1 if any case is worse than the baseline")
    a = ap.parse_args()
    if a.list:
        for k, (d, _) in CASES.items():
            print(f"{k:34s} {d}")
        return
    runner = S.find_gsrunner()
    if not runner or not S.BENCH.exists():
        sys.exit(f"need GSRunner ({runner}) and {S.BENCH}")
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, (desc, fn) in CASES.items():
        if a.only and a.only not in name and not (a.gate and name.startswith("control_")):
            continue
        work = OUT / name
        work.mkdir(exist_ok=True)
        gsr = work / "case.gsr"
        n = write_gsr(gsr, fn(random.Random(f"{a.seed}:{name}")))
        extra = {"PS2X_GSBENCH_SKIP": str(n - 1)} if name.startswith("control_") else None
        bmp = work / "ours.bmp"
        if bmp.exists():
            bmp.unlink()
        S.render(gsr, bmp, FBP, 1, extra=extra, w=W, h=H)
        ours = np.asarray(Image.open(bmp).convert("RGB"))[:H, :W]
        ref = S.oracle_frame(gsr, FBP, 1, CT32, None, work, runner, w=W, h=H)
        if ref is None:
            rows.append((name, "NOREF", 0, 0, 0, desc))
            print(f"{name:34s} NOREF")
            continue
        verdict, mx, n2, n8, d = grade(ours, ref)
        lit = int((ref.max(axis=2) > 0).sum())
        if lit == 0:
            verdict = "EMPTY"
        big = lambda arr: Image.fromarray(arr.astype(np.uint8)).resize((W * 4, H * 4), Image.NEAREST)
        big(ours).save(OUT / f"{name}_ours.png")
        big(ref).save(OUT / f"{name}_ref.png")
        big(np.clip(d * 8, 0, 255)).save(OUT / f"{name}_diff.png")
        rows.append((name, verdict, mx, n2, n8, desc))
        print(f"{name:34s} {verdict:5s} lit={lit:4d} max={mx:3d} px>2={n2:4d} px>8={n8:4d}  {desc}")
    ctrl = [r for r in rows if r[0].startswith("control_")]
    ok = bool(ctrl) and all(r[1] == "DIFF" for r in ctrl)
    lines = ["# GS feature matrix (ours vs PCSX2 SW)", "",
             f"control: {'OK (DIFF)' if ok else ('BLIND - control did not DIFF' if ctrl else 'not run')}", "",
             "| case | verdict | max | px>2 | px>8 | what |", "|---|---|---|---|---|---|"]
    lines += [f"| {r[0]} | {r[1]} | {r[2]} | {r[3]} | {r[4]} | {r[5]} |" for r in rows]
    (OUT / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    cnt = {}
    for r in rows:
        cnt[r[1]] = cnt.get(r[1], 0) + 1
    print(f"\n{cnt}  control {'OK' if ok else ('BLIND' if ctrl else 'not run')}  report: {OUT / 'report.md'}")

    # Gate: a case may not get worse than the saved baseline (known DIFFs stay allowed).
    if a.save_baseline:
        # merge, so --only X --save-baseline keeps every other case's entry
        merged = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}
        merged.update({r[0]: [r[1], r[4]] for r in rows})
        tmp = BASELINE.with_suffix(".tmp")
        tmp.write_text(json.dumps(merged, indent=1), encoding="utf-8")
        os.replace(tmp, BASELINE)
        print(f"baseline saved: {BASELINE}")
    if a.gate:
        if not BASELINE.exists():
            sys.exit(f"GATE FAIL: no baseline {BASELINE} (run --save-baseline once)")
        base = json.loads(BASELINE.read_text(encoding="utf-8"))
        worse = []
        for name, verdict, _mx, _n2, n8, _d in rows:
            if name not in base or name.startswith("control_"):
                continue
            bv, bn8 = base[name]
            if RANK.get(verdict, 9) > RANK.get(bv, 9) or n8 > bn8 * 1.05 + 5:
                worse.append(f"{name} {bv}/{bn8} -> {verdict}/{n8}")
        if not ok:
            worse.append("control did not DIFF (tool blind)")
        if worse:
            print("GATE FAIL: " + "; ".join(worse))
            sys.exit(1)
        print(f"GATE PASS: {len(rows)} cases, none worse than baseline")


if __name__ == "__main__":
    main()
