#!/usr/bin/env python3
"""
texmiss.py -- "drawn but never uploaded" texture detector.

Lists every texture / palette a draw READ from GS VRAM that nothing WROTE
(host upload, local->local copy, or a draw rendering into that memory).

Input is the JSONL the runtime streams with PS2X_TEXMISS_LOG=<path>
(ps2_gs_gpu.cpp, "[texmiss]" block), or a PCSX2 GS dump for calibration.

  python texmiss.py logs\\texmiss.jsonl
  python texmiss.py logs\\texmiss.jsonl --runlog run_log.txt
  python texmiss.py logs\\texmiss.jsonl --scene 1100 1130
  python texmiss.py --gsdump "snaps\\x.gs.zst"          (calibration, Part C)

Status per key:
  NEVER    no part of the read area was ever written      <- the strong signal
  PARTIAL  some of the area was never written (ever %)     <- lead only
  LATE     all written, but some only AFTER the first read (before %)

ADDRESS MATH -- copied from the runtime, NOT from PS2 hardware (layouts differ):
  ps2_gs_memory.h   PixelStorageTraits<psm>::Address / PageId / BlockId / ColumnId
  ps2_gs_memory.cpp BlockTable* / ColumnTable*, InitPageLookupTable
  ps2_gs_memory.cpp ReadPixel*/WritePixel* (CT24,T8H,T4HL,T4HH use the C32 layout)
  units: bp = block (TEX0.tbp0, BITBLTBUF.dbp; FRAME.fbp << 5),
         bw = pixels / 64, address = pixel index in that layout,
         bits = address * UnpackedBitWidth + BitOffset,
         byte = (bits / 8) & (4 MB - sizeof(packed)).
  Coverage is tracked per NIBBLE of the 4 MB VRAM.
  CLUT (CSM1) coords from GS::ReloadClutCacheCSM1, read at (cbp, bw=1, cpsm):
         4-bit: x = i & 7,                y = (i / 8) & 1
         8-bit: x = (i & 7) + 8*(i&0x10), y = (i & 0xE0)/16 + (i & 8 ? 1 : 0)
  Texel reads use the ReadVram layout. The sampler's page cache
  (GS::ReadTexturePageCache) copies whole pages from that same layout --
  hypothesis, not diffed.
"""

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np

LIMITS = [
    "upload counted at its TRXDIR write, not when the last pixel lands",
    "no UVs logged: read area = tw x th (narrowed only by REGION_CLAMP) -> PARTIAL can be false",
    "palette read stamped at draw time; real load is at the TEX0 write (cld)",
    "CSM2 (TEXCLUT) palettes not checked",
    "finds missing TEXTURES, not missing DRAWS",
]

MEM_BYTES = 4 * 1024 * 1024
NIBBLES = MEM_BYTES * 2
NEVER_WRITTEN = np.iinfo(np.int32).max

# --- ps2_gs_memory.cpp tables (verbatim) -----------------------------------
BLOCK_C32 = [
    [0, 1, 4, 5, 16, 17, 20, 21],
    [2, 3, 6, 7, 18, 19, 22, 23],
    [8, 9, 12, 13, 24, 25, 28, 29],
    [10, 11, 14, 15, 26, 27, 30, 31],
]
BLOCK_Z32 = [
    [24, 25, 28, 29, 8, 9, 12, 13],
    [26, 27, 30, 31, 10, 11, 14, 15],
    [16, 17, 20, 21, 0, 1, 4, 5],
    [18, 19, 22, 23, 2, 3, 6, 7],
]
BLOCK_C16 = [
    [0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
    [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31],
]
BLOCK_C16S = [
    [0, 2, 16, 18], [1, 3, 17, 19], [8, 10, 24, 26], [9, 11, 25, 27],
    [4, 6, 20, 22], [5, 7, 21, 23], [12, 14, 28, 30], [13, 15, 29, 31],
]
BLOCK_Z16 = [
    [24, 26, 16, 18], [25, 27, 17, 19], [28, 30, 20, 22], [29, 31, 21, 23],
    [8, 10, 0, 2], [9, 11, 1, 3], [12, 14, 4, 6], [13, 15, 5, 7],
]
BLOCK_Z16S = [
    [24, 26, 8, 10], [25, 27, 9, 11], [16, 18, 0, 2], [17, 19, 1, 3],
    [28, 30, 12, 14], [29, 31, 13, 15], [20, 22, 4, 6], [21, 23, 5, 7],
]
BLOCK_P8 = [
    [0, 1, 4, 5, 16, 17, 20, 21],
    [2, 3, 6, 7, 18, 19, 22, 23],
    [8, 9, 12, 13, 24, 25, 28, 29],
    [10, 11, 14, 15, 26, 27, 30, 31],
]
BLOCK_P4 = [
    [0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
    [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31],
]
COL_32 = [
    [0, 1, 4, 5, 8, 9, 12, 13],
    [2, 3, 6, 7, 10, 11, 14, 15],
    [16, 17, 20, 21, 24, 25, 28, 29],
    [18, 19, 22, 23, 26, 27, 30, 31],
    [32, 33, 36, 37, 40, 41, 44, 45],
    [34, 35, 38, 39, 42, 43, 46, 47],
    [48, 49, 52, 53, 56, 57, 60, 61],
    [50, 51, 54, 55, 58, 59, 62, 63],
]
COL_16 = [
    [0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27],
    [4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31],
    [32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59],
    [36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63],
    [64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91],
    [68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95],
    [96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123],
    [100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127],
]
COL_8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255],
]
COL_4 = [
    [0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110],
    [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126],
    [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47],
    [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63],
    [192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174],
    [208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190],
    [129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239],
    [145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255],
    [256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366],
    [272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382],
    [321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303],
    [337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319],
    [448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430],
    [464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446],
    [385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495],
    [401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511],
]


class Traits:
    """PixelStorageTraits<psm>: page / block / column extents are (x, y)."""

    def __init__(self, page, block, col, block_table, col_table):
        self.pex, self.pey = page
        self.bex, self.bey = block
        self.cex, self.cey = col
        self.bt = np.array(block_table, dtype=np.int64)  # [y][x]
        self.ct = np.array(col_table, dtype=np.int64)    # [y][x]
        assert self.bt.shape == (self.bey, self.bex)
        assert self.ct.shape == (self.cey, self.cex)
        assert self.bex * self.bey == 32
        self.pixels_per_block = self.cex * self.cey
        self.pixels_per_page = self.pex * self.pey


TRAITS = {
    "C32": Traits((64, 32), (8, 4), (8, 8), BLOCK_C32, COL_32),
    "Z32": Traits((64, 32), (8, 4), (8, 8), BLOCK_Z32, COL_32),
    "C16": Traits((64, 64), (4, 8), (16, 8), BLOCK_C16, COL_16),
    "C16S": Traits((64, 64), (4, 8), (16, 8), BLOCK_C16S, COL_16),
    "Z16": Traits((64, 64), (4, 8), (16, 8), BLOCK_Z16, COL_16),
    "Z16S": Traits((64, 64), (4, 8), (16, 8), BLOCK_Z16S, COL_16),
    "P8": Traits((128, 64), (8, 4), (16, 16), BLOCK_P8, COL_8),
    "P4": Traits((128, 128), (4, 8), (32, 16), BLOCK_P4, COL_4),
}

# psm -> (name, layout traits, UnpackedBitWidth, BitOffset, packed bits)
PSM = {
    0x00: ("CT32", "C32", 32, 0, 32),
    0x01: ("CT24", "C32", 32, 0, 24),
    0x02: ("CT16", "C16", 16, 0, 16),
    0x0A: ("CT16S", "C16S", 16, 0, 16),
    0x13: ("T8", "P8", 8, 0, 8),
    0x14: ("T4", "P4", 4, 0, 4),
    0x1B: ("T8H", "C32", 32, 24, 8),
    0x24: ("T4HL", "C32", 32, 24, 4),
    0x2C: ("T4HH", "C32", 32, 28, 4),
    0x30: ("Z32", "Z32", 32, 0, 32),
    0x31: ("Z24", "Z32", 32, 0, 24),
    0x32: ("Z16", "Z16", 16, 0, 16),
    0x3A: ("Z16S", "Z16S", 16, 0, 16),
}
INDEXED_BITS = {0x13: 8, 0x14: 4, 0x1B: 8, 0x24: 4, 0x2C: 4}


def psm_name(psm):
    info = PSM.get(psm)
    return info[0] if info else "0x%02X" % psm


def nibbles(psm, bp, bw, xs, ys):
    """Nibble indices touched by pixels (xs, ys) at (bp, bw, psm)."""
    info = PSM.get(psm)
    if info is None or xs.size == 0:
        return None  # ReadPixelNull / WritePixelNull
    _, layout, unpacked, bit_offset, packed = info
    tr = TRAITS[layout]
    xs = xs.astype(np.int64)
    ys = ys.astype(np.int64)
    # PageId
    base_page = bp // 32
    row = (ys // tr.pey) * ((bw * 64) // tr.pex)
    col = xs // tr.pex
    page = base_page + row + col
    # Address: block % 32, x % page, y % page, then the page lookup table
    block = bp % 32
    yy = ys % tr.pey
    xx = xs % tr.pex
    block_id = tr.bt[(yy // tr.cey) % tr.bey, (xx // tr.cex) % tr.bex]
    column_id = tr.ct[yy % tr.cey, xx % tr.cex]
    address = page * tr.pixels_per_page + (block + block_id) * tr.pixels_per_block + column_id
    bits = address * unpacked + bit_offset
    size_of_packed = 4 if packed > 16 else (2 if packed == 16 else 1)
    byte = (bits >> 3) & (MEM_BYTES - size_of_packed)
    nib0 = byte * 2 + ((bits & 7) >> 2)
    count = max(1, packed // 4)
    if count > 1:
        nib0 = (nib0[:, None] + np.arange(count, dtype=np.int64)).ravel()
    return nib0 % NIBBLES


def rect_nibbles(psm, bp, bw, x0, y0, w, h):
    if w <= 0 or h <= 0:
        return None
    w = min(w, 2048)
    h = min(h, 2048)
    ys, xs = np.mgrid[y0:y0 + h, x0:x0 + w]
    return nibbles(psm, bp, bw, xs.ravel(), ys.ravel())


def clut_coords(index_bits):
    """GS::ReloadClutCacheCSM1 source coordinates."""
    xs, ys = [], []
    if index_bits == 4:
        for i in range(16):
            xs.append(i & 7)
            ys.append((i // 8) & 1)
    else:
        for i in range(256):
            xs.append((i & 7) + (8 if i & 0x10 else 0))
            ys.append((i & 0xE0) // 16 + (1 if i & 8 else 0))
    return np.array(xs, dtype=np.int64), np.array(ys, dtype=np.int64)


CLUT_COORDS = {4: clut_coords(4), 8: clut_coords(8)}


class Checker:
    def __init__(self, scene=None, control=None):
        self.control = control
        self.first_write = np.full(NIBBLES, NEVER_WRITTEN, dtype=np.int32)
        self.seen_writes = set()
        self.reads = {}   # key -> dict
        self.scene = scene
        self.counts = {"D": 0, "X": 0, "P": 0, "LOSS": 0, "lost_events": 0, "PAUSED": 0,
                       "writes_marked": 0, "draw_writes_skipped_fbmsk": 0}
        self.t_first = None
        self.t_last = None

    # -- writes -----------------------------------------------------------
    def write(self, order, key, psm, bp, bw, x0, y0, w, h):
        if key in self.seen_writes:
            return  # an earlier identical write already holds the lower order
        self.seen_writes.add(key)
        nib = rect_nibbles(psm, bp, bw, x0, y0, w, h)
        if nib is None:
            return
        cur = self.first_write[nib]
        self.first_write[nib] = np.where(cur == NEVER_WRITTEN, order, cur)
        self.counts["writes_marked"] += 1

    # -- reads ------------------------------------------------------------
    def read(self, order, t, v, key, desc, region):
        if self.scene and not (self.scene[0] <= t <= self.scene[1]):
            return
        r = self.reads.get(key)
        if r is None:
            self.reads[key] = {"order": order, "t": t, "v": v, "draws": 1, "desc": desc,
                               "region": region, "t_last": t}
            return
        r["draws"] += 1
        r["t_last"] = t
        if region[0] == "rect":
            a, b = r["region"], region
            r["region"] = ("rect", a[1], a[2], a[3],
                           min(a[4], b[4]), min(a[5], b[5]), max(a[6], b[6]), max(a[7], b[7]))

    # -- events -----------------------------------------------------------
    def event(self, order, e):
        k = e.get("k")
        t = e.get("t", 0.0)
        if k in ("D", "X", "P"):
            self.t_first = t if self.t_first is None else self.t_first
            self.t_last = t
        if k == "D":
            self.counts["D"] += 1
            self.draw(order, e)
        elif k == "X":
            self.counts["X"] += 1
            self.transfer(order, e)
        elif k == "P":
            self.counts["P"] += 1
        elif k == "LOSS":
            self.counts["LOSS"] += 1
            self.counts["lost_events"] += e.get("dropped", 0)
        elif k == "PAUSED":
            self.counts["PAUSED"] += 1

    def transfer(self, order, e):
        xdir = e["xdir"]
        if self.control == "drop-uploads" and xdir == 0:
            return  # negative control: every uploaded key must turn NEVER/PARTIAL
        if xdir in (0, 2):
            self.write(order, ("X", e["dpsm"], e["dbp"], e["dbw"], e["dx"], e["dy"], e["w"], e["h"]),
                       e["dpsm"], e["dbp"], e["dbw"], e["dx"], e["dy"], e["w"], e["h"])
        if xdir == 2:
            desc = "copy src sbp=0x%04X sbw=%d %s %dx%d@(%d,%d)" % (
                e["sbp"], e["sbw"], psm_name(e["spsm"]), e["w"], e["h"], e["sx"], e["sy"])
            key = ("copy", e["sbp"], e["sbw"], e["spsm"], e["sx"], e["sy"], e["w"], e["h"])
            self.read(order, e["t"], e["v"], key, desc,
                      ("rect", e["spsm"], e["sbp"], e["sbw"], e["sx"], e["sy"],
                       e["sx"] + e["w"], e["sy"] + e["h"]))

    def draw(self, order, e):
        # render target write: vertex bbox (raw XYZ/16) minus XYOFFSET, clipped to scissor
        if (e["fbmsk"] & 0xFFFFFFFF) != 0xFFFFFFFF:
            ofx, ofy = e["ofx"] >> 4, e["ofy"] >> 4
            sx0, sy0, sx1, sy1 = e["sc"]
            x0 = max(int(e["x0"]) - ofx, sx0, 0)
            y0 = max(int(e["y0"]) - ofy, sy0, 0)
            x1 = min(int(e["x1"]) - ofx, sx1)
            y1 = min(int(e["y1"]) - ofy, sy1)
            if x1 >= x0 and y1 >= y0:
                w, h = x1 - x0 + 1, y1 - y0 + 1
                bp = e["fbp"] << 5
                self.write(order, ("D", e["fpsm"], bp, e["fbw"], x0, y0, w, h),
                           e["fpsm"], bp, max(e["fbw"], 1), x0, y0, w, h)
        else:
            self.counts["draw_writes_skipped_fbmsk"] += 1

        if not e["tme"]:
            return
        tw, th = 1 << e["tw"], 1 << e["th"]
        clamp = e.get("clamp", 0)
        wms, wmt = clamp & 3, (clamp >> 2) & 3
        u0, u1, v0, v1 = 0, tw, 0, th
        if wms == 2:  # REGION_CLAMP
            u0, u1 = max(0, (clamp >> 4) & 0x3FF), min(tw, ((clamp >> 14) & 0x3FF) + 1)
        if wmt == 2:
            v0, v1 = max(0, (clamp >> 24) & 0x3FF), min(th, ((clamp >> 34) & 0x3FF) + 1)
        if u1 <= u0 or v1 <= v0:
            u0, u1, v0, v1 = 0, tw, 0, th
        psm = e["psm"]
        desc = "tex tbp=0x%04X tbw=%d %s %dx%d" % (e["tbp"], e["tbw"], psm_name(psm), tw, th)
        key = ("tex", e["tbp"], e["tbw"], psm, tw, th)
        self.read(order, e["t"], e["v"], key, desc,
                  ("rect", psm, e["tbp"], max(e["tbw"], 1), u0, v0, u1, v1))

        bits = INDEXED_BITS.get(psm)
        if bits and e["csm"] == 0:
            desc = "clut cbp=0x%04X %s %d-bit (for %s)" % (e["cbp"], psm_name(e["cpsm"]), bits, psm_name(psm))
            key = ("clut", e["cbp"], e["cpsm"], bits)
            self.read(order, e["t"], e["v"], key, desc, ("clut", e["cpsm"], e["cbp"], bits))

    # -- verdict ----------------------------------------------------------
    def region_nibbles(self, region):
        if region[0] == "rect":
            _, psm, bp, bw, u0, v0, u1, v1 = region
            return rect_nibbles(psm, bp, bw, u0, v0, u1 - u0, v1 - v0)
        _, cpsm, cbp, bits = region
        xs, ys = CLUT_COORDS[bits]
        return nibbles(cpsm, cbp, 1, xs, ys)

    def results(self):
        out = []
        for key, r in self.reads.items():
            nib = self.region_nibbles(r["region"])
            if nib is None:
                continue
            fw = self.first_write[nib]
            ever = float(np.mean(fw != NEVER_WRITTEN))
            before = float(np.mean(fw < r["order"]))
            if ever == 0.0:
                status = "NEVER"
            elif ever < 1.0:
                status = "PARTIAL"
            elif before < 1.0:
                status = "LATE"
            else:
                status = "OK"
            region = r["region"]
            if region[0] == "rect":
                area = "u%d-%d v%d-%d" % (region[4], region[6] - 1, region[5], region[7] - 1)
            else:
                area = "%d entries" % (1 << region[3])
            out.append({"status": status, "t": r["t"], "t_last": r["t_last"], "v": r["v"],
                        "draws": r["draws"], "what": r["desc"], "area": area,
                        "ever": ever, "before": before, "kind": key[0]})
        out.sort(key=lambda x: x["t"])
        return out


# --- inputs ------------------------------------------------------------------
def load_jsonl(path, checker):
    bad = 0
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for order, line in enumerate(f):
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
            except ValueError:
                bad += 1  # a run killed mid-write leaves one torn last line
                continue
            checker.event(order, e)
    return bad


def load_gsdump(path, checker):
    """Part C: turn a PCSX2 GS dump into the same event schema."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gsdump_draws as D
    import gsdump_parse as G

    events = []

    class State(D.GsState):
        def __init__(self):
            super().__init__()
            self.clamp = [0, 0]
            self.scissor = [0, 0]
            self.frame_full = [0, 0]
            self.bitbltbuf = 0
            self.trxpos = 0
            self.trxreg = 0

        def emit(self, verts):
            super().emit(verts)
            ctx = self.prim["ctxt"]
            t0 = self.tex0[ctx] or {"tbp0": 0, "tbw": 0, "psm": 0, "tw": 1, "th": 1,
                                    "cbp": 0, "cpsm": 0, "csm": 0, "csa": 0, "cld": 0}
            fr = self.frame_full[ctx]
            sc = self.scissor[ctx]
            ofx, ofy = self.xyoffset[ctx]
            xs = [v["x"] / 16.0 for v in verts]
            ys = [v["y"] / 16.0 for v in verts]
            events.append({
                "k": "D", "t": float(self.frame_index), "v": self.frame_index,
                "prim": self.prim["type"], "tme": self.prim["tme"], "ctxt": ctx, "n": len(verts),
                "fbp": fr & 0x1FF, "fbw": (fr >> 16) & 0x3F, "fpsm": (fr >> 24) & 0x3F,
                "fbmsk": (fr >> 32) & 0xFFFFFFFF,
                "x0": min(xs), "y0": min(ys), "x1": max(xs), "y1": max(ys),
                "sc": [sc & 0x7FF, (sc >> 32) & 0x7FF, (sc >> 16) & 0x7FF, (sc >> 48) & 0x7FF],
                "ofx": ofx, "ofy": ofy, "clamp": self.clamp[ctx],
                "tbp": t0["tbp0"], "tbw": t0["tbw"], "psm": t0["psm"],
                "tw": t0["tw"].bit_length() - 1, "th": t0["th"].bit_length() - 1,
                "cbp": t0["cbp"], "cpsm": t0["cpsm"], "csm": t0["csm"], "csa": t0["csa"], "cld": t0["cld"],
            })

    original_apply = D.apply_reg

    def apply_reg(st, addr, data):
        original_apply(st, addr, data)
        if addr in (0x08, 0x09):
            st.clamp[addr - 0x08] = data
        elif addr in (0x40, 0x41):
            st.scissor[addr - 0x40] = data
        elif addr in (0x4C, 0x4D):
            st.frame_full[addr - 0x4C] = data
        elif addr == 0x50:
            st.bitbltbuf = data
        elif addr == 0x51:
            st.trxpos = data
        elif addr == 0x52:
            st.trxreg = data
        elif addr == 0x53:
            b, p, r = st.bitbltbuf, st.trxpos, st.trxreg
            events.append({
                "k": "X", "t": float(st.frame_index), "v": st.frame_index, "xdir": data & 3,
                "sbp": b & 0x3FFF, "sbw": (b >> 16) & 0x3F, "spsm": (b >> 24) & 0x3F,
                "sx": p & 0x7FF, "sy": (p >> 16) & 0x7FF,
                "dbp": (b >> 32) & 0x3FFF, "dbw": (b >> 48) & 0x3F, "dpsm": (b >> 56) & 0x3F,
                "dx": (p >> 32) & 0x7FF, "dy": (p >> 48) & 0x7FF,
                "w": r & 0xFFF, "h": (r >> 32) & 0xFFF, "px": 0,
            })

    D.apply_reg = apply_reg
    raw = G.decompress(path, path.read_bytes())
    info, _state, _regs, reader = G.parse_container(raw)
    st = State()
    for p in G.parse_packets(reader):
        if p["id"] == G.PACKET_TRANSFER:
            D.process_packet(st, p["data"])
        elif p["id"] == G.PACKET_VSYNC:
            st.frame_index += 1
    D.apply_reg = original_apply

    for order, e in enumerate(events):
        checker.event(order, e)
    print("gsdump serial=%s frames=%d draws=%d transfers=%d  (register state before the "
          "first write of each register is taken as 0)"
          % (info.get("serial"), st.frame_index + 1,
             sum(1 for e in events if e["k"] == "D"), sum(1 for e in events if e["k"] == "X")))


# --- run_log alignment -----------------------------------------------------------
WATCHDOG = re.compile(r"\[watchdog\] t=(\d+)s")


def read_runlog(path):
    data = Path(path).read_bytes()
    if data[:2] in (b"\xff\xfe", b"\xfe\xff") or data[1:2] == b"\x00":
        text = data.decode("utf-16", errors="replace")
    else:
        text = data.decode("utf-8", errors="replace")
    offset = None
    wd = 0
    loads = []  # (watchdog t, line)
    for line in text.splitlines():
        m = WATCHDOG.search(line)
        if m:
            wd = int(m.group(1))
            continue
        if offset is None and "[texmiss] ACTIVE" in line:
            offset = wd
        elif "[ARKD:load]" in line:
            loads.append((wd, line.strip()[:110]))
    return offset, loads


def nearest_load(loads, t_wd):
    best = None
    for wd, line in loads:
        if wd <= t_wd:
            best = (wd, line)
        else:
            break
    return best


# --- main ----------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", type=Path, help="PS2X_TEXMISS_LOG jsonl")
    ap.add_argument("--gsdump", type=Path, help="PCSX2 .gs/.gs.zst dump instead of a log (calibration)")
    ap.add_argument("--runlog", type=Path, help="run_log.txt (UTF-16LE ok): add nearest [ARKD:load]")
    ap.add_argument("--scene", nargs=2, type=float, metavar=("T0", "T1"),
                    help="only reads whose time is in [T0, T1] (log seconds; watchdog seconds with --runlog)")
    ap.add_argument("--all", action="store_true", help="also list OK keys")
    ap.add_argument("--kind", choices=["tex", "clut", "copy"], help="only this read kind")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--control", choices=["drop-uploads"],
                    help="negative control: ignore host uploads; uploaded keys MUST stop being OK")
    args = ap.parse_args()
    if not args.log and not args.gsdump:
        ap.error("give a log path or --gsdump")

    offset, loads = (None, [])
    if args.runlog:
        offset, loads = read_runlog(args.runlog)
        if offset is None:
            print("WARNING: no '[texmiss] ACTIVE' line in run log -- times NOT aligned", file=sys.stderr)

    scene = None
    if args.scene:
        shift = offset or 0
        scene = (args.scene[0] - shift, args.scene[1] - shift)

    checker = Checker(scene, args.control)
    bad = 0
    if args.gsdump:
        load_gsdump(args.gsdump, checker)
    else:
        bad = load_jsonl(args.log, checker)

    rows = checker.results()
    for r in rows:
        r["t_wd"] = r["t"] + offset if offset is not None else None
        if loads and r["t_wd"] is not None:
            hit = nearest_load(loads, r["t_wd"])
            r["load"] = hit[1] if hit else None
    if args.kind:
        rows = [r for r in rows if r["kind"] == args.kind]

    tally = {}
    for r in rows:
        tally[r["status"]] = tally.get(r["status"], 0) + 1
    shown = rows if args.all else [r for r in rows if r["status"] != "OK"]

    if args.json:
        json.dump({"counts": checker.counts, "tally": tally, "limits": LIMITS, "rows": shown,
                   "torn_lines": bad}, sys.stdout, indent=1)
        print()
        return

    c = checker.counts
    print("events: draws=%d transfers=%d presents=%d | LOSS lines=%d lost=%d | PAUSED=%d | torn=%d"
          % (c["D"], c["X"], c["P"], c["LOSS"], c["lost_events"], c["PAUSED"], bad))
    if checker.t_first is not None:
        print("time: %.1f .. %.1f s%s" % (checker.t_first, checker.t_last,
                                          "  (watchdog offset %+d)" % offset if offset is not None else ""))
    print("keys read: %d   %s" % (len(rows), "  ".join("%s=%d" % kv for kv in sorted(tally.items()))))
    print("limits:")
    for lim in LIMITS:
        print("  - " + lim)
    if c["lost_events"]:
        print("!! %d events LOST to ring overflow -- a NEVER can be a lost upload" % c["lost_events"])
    if rows and tally.get("NEVER", 0) == len(rows):
        print("!! EVERY key is NEVER -- degenerate: suspect the checker, not the game")
    if not rows:
        print("!! no reads at all -- degenerate: suspect the log / filters")
    print()

    print("%-7s %8s %6s %6s %6s %6s  %s" % ("status", "t", "vsync", "draws", "ever%", "pre%", "what / area"))
    for r in shown:
        t = r["t_wd"] if r["t_wd"] is not None else r["t"]
        print("%-7s %8.1f %6d %6d %6.1f %6.1f  %s  [%s]"
              % (r["status"], t, r["v"], r["draws"], 100 * r["ever"], 100 * r["before"], r["what"], r["area"]))
        if r.get("load"):
            print("%38s after: %s" % ("", r["load"]))


if __name__ == "__main__":
    main()
