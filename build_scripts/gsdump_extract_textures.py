#!/usr/bin/env python3
"""
gsdump_extract_textures.py -- pull actual texture images (as PNGs) out of a
PCSX2 GS dump, by replaying its GIF packet stream into a simulated 4 MB GS
local memory and decoding whatever TEX0/CLUT state each draw bound.

Why this exists
----------------
gsdump_draws.py already decodes the GIF stream into per-draw GS state
(TEX0/CLUT/etc) -- see [[project_capp_scene_identification]] and the Stage
5.11 tooling. It never turns that state into pixels. texmiss.py separately
has fully calibrated VRAM address math (block/column swizzle tables per PSM),
but only uses it to compute which NIBBLES a read/write touches, not to move
actual bytes. This script reuses both: texmiss's address math to place/read
real bytes in a simulated VRAM buffer, gsdump_draws's GIF walker to drive it.

The upload payload PCSX2 records (GIFtag FLG=IMAGE) is the CPU-side raster
stream: for a WxH host->local BITBLTBUF transfer, pixel (x,y) is at payload
offset (y*w+x)*bytes_per_pixel, DESTINATION-packed (i.e. already in dpsm's
packed representation -- 1 byte/px for T8, 4 bytes/px for CT32, etc). GS
hardware applies the block/column swizzle when it writes that stream into
local memory; we replicate that with the exact same address function
texmiss.py uses (itself calibrated against a real PCSX2 oracle dump per
project_texmiss_detector.md), so CLUT reads (which walk a swizzled 16x16
block via GS::ReloadClutCacheCSM1's CLUT_COORDS) come out correct too.

VALIDATED FORMATS: CT32, CT24, CT16, CT16S (color, byte/halfword aligned)
and T8 (8-bit index, byte aligned). T4/T8H/T4HL/T4HH are NOT implemented --
they require sub-byte nibble packing this script does not do yet; a draw
using one of those formats is skipped with a clear message, not guessed at.

Usage
-----
  python gsdump_extract_textures.py dump.gs.zst --list
  python gsdump_extract_textures.py dump.gs.zst --draw 1 --out draw1.png
  python gsdump_extract_textures.py dump.gs.zst --all --outdir textures/
  python gsdump_extract_textures.py dump.gs.zst --all --outdir textures/ --min-size 32

`--list` costs nothing extra (same walk as gsdump_draws.py --all) and shows
which draw indices are worth pulling. `--all` walks once, decoding every
*distinct* (tbp0,tbw,psm,tw,th,cbp,cpsm,csa) combination the first time it is
seen (VRAM at that point in the replay), which is what you want for a
texture atlas -- distinct combos, not one image per draw call.
"""

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gsdump_draws as D
import gsdump_parse as G
import texmiss as T

# psm -> bytes/pixel, for the byte-aligned formats we support writing/reading.
BYTE_PSM = {
    0x00: 4,  # CT32
    0x01: 4,  # CT24 (stored as 32-bit slot, top byte unused on read-back)
    0x02: 2,  # CT16
    0x0A: 2,  # CT16S
    0x13: 1,  # T8
}


def pixel_byte_offsets(psm, bp, bw, xs, ys):
    """Same address math as texmiss.nibbles(), stopped at the byte boundary.
    Only valid for BYTE_PSM formats (asserted by the caller)."""
    info = T.PSM[psm]
    _, layout, unpacked, bit_offset, packed = info
    tr = T.TRAITS[layout]
    xs = xs.astype(np.int64)
    ys = ys.astype(np.int64)
    base_page = bp // 32
    row = (ys // tr.pey) * ((bw * 64) // tr.pex)
    col = xs // tr.pex
    page = base_page + row + col
    block = bp % 32
    yy = ys % tr.pey
    xx = xs % tr.pex
    block_id = tr.bt[(yy // tr.cey) % tr.bey, (xx // tr.cex) % tr.bex]
    column_id = tr.ct[yy % tr.cey, xx % tr.cex]
    address = page * tr.pixels_per_page + (block + block_id) * tr.pixels_per_block + column_id
    bits = address * unpacked + bit_offset
    size_of_packed = 4 if packed > 16 else (2 if packed == 16 else 1)
    byte = (bits >> 3) & (T.MEM_BYTES - size_of_packed)
    return byte


class Vram:
    def __init__(self):
        self.mem = bytearray(T.MEM_BYTES)

    def write_rect(self, psm, bp, bw, x0, y0, w, h, payload):
        bpp = BYTE_PSM.get(psm)
        if bpp is None:
            return False
        need = w * h * bpp
        if len(payload) < need:
            return False  # torn/short transfer; do not write partial garbage
        ys, xs = np.mgrid[y0:y0 + h, x0:x0 + w]
        offs = pixel_byte_offsets(psm, bp, bw, xs.ravel(), ys.ravel())
        buf = np.frombuffer(self.mem, dtype=np.uint8)
        src = np.frombuffer(bytes(payload[:need]), dtype=np.uint8).reshape(-1, bpp)
        for b in range(bpp):
            buf_writable = np.frombuffer(self.mem, dtype=np.uint8)
            np.put(buf_writable, offs + b, src[:, b])
        return True

    def read_rect(self, psm, bp, bw, x0, y0, w, h):
        bpp = BYTE_PSM[psm]
        ys, xs = np.mgrid[y0:y0 + h, x0:x0 + w]
        offs = pixel_byte_offsets(psm, bp, bw, xs.ravel(), ys.ravel())
        buf = np.frombuffer(self.mem, dtype=np.uint8)
        out = np.empty((w * h, bpp), dtype=np.uint8)
        for b in range(bpp):
            out[:, b] = buf[offs + b]
        return out.reshape(h, w, bpp)

    def read_clut(self, cpsm, cbp, bits):
        bpp = BYTE_PSM.get(cpsm)
        if bpp is None:
            return None
        xs, ys = T.CLUT_COORDS[bits]
        offs = pixel_byte_offsets(cpsm, cbp, 1, xs, ys)
        buf = np.frombuffer(self.mem, dtype=np.uint8)
        out = np.empty((len(xs), bpp), dtype=np.uint8)
        for b in range(bpp):
            out[:, b] = buf[offs + b]
        return out


def to_rgba(raw, psm):
    """raw: (...,bpp) uint8 array in this psm's packing -> (...,4) uint8 RGBA."""
    if psm in (0x00, 0x01):  # CT32 / CT24
        rgba = raw.astype(np.uint16).copy()
        r, g, b, a = raw[..., 0], raw[..., 1], raw[..., 2], raw[..., 3]
        alpha = np.minimum(a.astype(np.uint16) * 2, 255).astype(np.uint8)
        if psm == 0x01:
            alpha = np.full_like(a, 255)
        return np.stack([r, g, b, alpha], axis=-1)
    if psm in (0x02, 0x0A):  # CT16 / CT16S, 5-5-5-1
        v = raw[..., 0].astype(np.uint16) | (raw[..., 1].astype(np.uint16) << 8)
        r = ((v >> 0) & 0x1F).astype(np.uint16) * 255 // 31
        g = ((v >> 5) & 0x1F).astype(np.uint16) * 255 // 31
        b = ((v >> 10) & 0x1F).astype(np.uint16) * 255 // 31
        a = np.where((v >> 15) & 1, 255, 0).astype(np.uint16)
        return np.stack([r, g, b, a], axis=-1).astype(np.uint8)
    raise NotImplementedError("to_rgba: unsupported psm 0x%02X" % psm)


def decode_texture(vram, tex0):
    """tex0: dict from gsdump_draws.decode_tex0(). Returns (H,W,4) uint8 or
    None with a reason string if the format isn't supported yet."""
    psm = tex0["psm"]
    tw, th = tex0["tw"], tex0["th"]
    bits = T.INDEXED_BITS.get(psm)
    if bits:
        if psm != 0x13:
            return None, "indexed psm 0x%02X (only T8 implemented, not T4/T8H/T4HL/T4HH)" % psm
        idx = vram.read_rect(psm, tex0["tbp0"], max(tex0["tbw"], 1), 0, 0, tw, th)[..., 0]
        clut = vram.read_clut(tex0["cpsm"], tex0["cbp"], bits)
        if clut is None:
            return None, "clut format 0x%02X not supported" % tex0["cpsm"]
        clut_rgba = to_rgba(clut, tex0["cpsm"])
        return clut_rgba[idx], None
    if psm not in BYTE_PSM:
        return None, "psm 0x%02X not implemented" % psm
    raw = vram.read_rect(psm, tex0["tbp0"], max(tex0["tbw"], 1), 0, 0, tw, th)
    return to_rgba(raw, psm), None


def replay(dump_path, on_draw=None, on_upload=None):
    """Walk the dump once, maintaining VRAM. Calls on_draw(vram, tex0, frame,
    draw_index) for every textured draw and on_upload(...) after every
    successful host->local write, in stream order."""
    raw = G.decompress(dump_path, dump_path.read_bytes())
    info, _state, _regs, reader = G.parse_container(raw)
    packets = G.parse_packets(reader)

    vram = Vram()
    st = D.GsState()
    draw_index = [0]

    original_emit = D.GsState.emit

    def emit(self, verts):
        original_emit(self, verts)
        draw_index[0] += 1
        if on_draw is None:
            return
        ctx = self.prim["ctxt"]
        if self.prim["tme"] and self.tex0[ctx] is not None:
            on_draw(vram, self.tex0[ctx], self.frame_index, draw_index[0] - 1)

    D.GsState.emit = emit
    try:
        for p in packets:
            if p["id"] == G.PACKET_TRANSFER:
                D.process_packet(st, p["data"])
                if st.xfers:
                    x = st.xfers[-1]
                    if "image" in x and "trxdir" in x and x["trxdir"] == 0 and "consumed" not in x:
                        b = x["bitbltbuf"]
                        r = x.get("trxreg", 0)
                        p_ = x.get("trxpos", 0)
                        dpsm = D.bits(b, 56, 6)
                        dbp = D.bits(b, 32, 14)
                        dbw = D.bits(b, 48, 6)
                        dx = D.bits(p_, 32, 16)
                        dy = D.bits(p_, 48, 16)
                        w = D.bits(r, 0, 12)
                        h = D.bits(r, 32, 12)
                        if w and h:
                            ok = vram.write_rect(dpsm, dbp, dbw, dx, dy, w, h, x["image"])
                            if on_upload:
                                on_upload(dpsm, dbp, dbw, w, h, ok)
                        x["consumed"] = True
            elif p["id"] == G.PACKET_VSYNC:
                st.frame_index += 1
    finally:
        D.GsState.emit = original_emit

    return info, vram, draw_index[0]


def tex_key(tex0):
    return (tex0["tbp0"], tex0["tbw"], tex0["psm"], tex0["tw"], tex0["th"],
            tex0["cbp"], tex0["cpsm"], tex0["csa"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--list", action="store_true", help="list textured draws (index, frame, TEX0) and exit")
    ap.add_argument("--draw", type=int, help="decode only this draw index (from --list)")
    ap.add_argument("--all", action="store_true", help="decode every distinct texture state seen")
    ap.add_argument("--out", type=Path, help="output PNG for --draw")
    ap.add_argument("--outdir", type=Path, help="output directory for --all")
    ap.add_argument("--min-size", type=int, default=0, help="skip textures with tw or th below this")
    ap.add_argument("--limit", type=int, default=200, help="cap for --list")
    args = ap.parse_args()

    from PIL import Image

    if args.list:
        seen = []

        def on_draw(vram, tex0, frame, idx):
            seen.append((idx, frame, tex0))

        info, vram, ndraws = replay(args.dump, on_draw=on_draw)
        print("serial=%s draws=%d textured=%d" % (info.get("serial"), ndraws, len(seen)))
        for idx, frame, tex0 in seen[: args.limit]:
            print("[%5d] f%d %s" % (idx, frame, D.tex0_str(tex0)))
        return

    if args.draw is not None:
        target = args.draw
        result = {}

        def on_draw(vram, tex0, frame, idx):
            if idx == target and "img" not in result:
                img, err = decode_texture(vram, tex0)
                result["img"] = img
                result["err"] = err
                result["tex0"] = tex0

        replay(args.dump, on_draw=on_draw)
        if "img" not in result:
            sys.exit("draw index %d not found or not textured" % target)
        if result["img"] is None:
            sys.exit("draw %d: %s" % (target, result["err"]))
        out = args.out or Path("draw%d.png" % target)
        Image.fromarray(result["img"], "RGBA").save(out)
        print("wrote %s (%s)" % (out, D.tex0_str(result["tex0"])))
        return

    if args.all:
        outdir = args.outdir or Path("gsdump_textures")
        outdir.mkdir(parents=True, exist_ok=True)
        done = {}
        skipped = {}

        def on_draw(vram, tex0, frame, idx):
            if tex0["tw"] < args.min_size or tex0["th"] < args.min_size:
                return
            key = tex_key(tex0)
            if key in done or key in skipped:
                return
            img, err = decode_texture(vram, tex0)
            if img is None:
                skipped[key] = err
                return
            name = "tbp%04x_%dx%d_psm%02x_cbp%04x.png" % (
                tex0["tbp0"], tex0["tw"], tex0["th"], tex0["psm"], tex0["cbp"])
            Image.fromarray(img, "RGBA").save(outdir / name)
            done[key] = name

        info, vram, ndraws = replay(args.dump, on_draw=on_draw)
        print("serial=%s draws=%d  wrote=%d distinct textures to %s  skipped=%d"
              % (info.get("serial"), ndraws, len(done), outdir, len(skipped)))
        if skipped:
            reasons = {}
            for err in skipped.values():
                reasons[err] = reasons.get(err, 0) + 1
            print("skipped reasons:")
            for err, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
                print("  %4d  %s" % (n, err))
        return

    ap.error("give --list, --draw N, or --all")


if __name__ == "__main__":
    main()
