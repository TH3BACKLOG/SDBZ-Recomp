#!/usr/bin/env python3
"""
gsdump_draws.py -- decode the GIF stream inside a PCSX2 GS dump into a list of
draw primitives, so a specific on-screen object can be identified by its screen
rectangle and the exact GS state that produced it.

Stage 5.11 context: the memory-card screen's red box renders opaque black in our
runtime. Every probe so far has had to GUESS which texture/CLUT the box uses.
This reads PCSX2's own recording of the frame and answers it directly.

  python gsdump_draws.py dump.gs.zst --sprites
  python gsdump_draws.py dump.gs.zst --rect 23 301 484 419
  python gsdump_draws.py dump.gs.zst --frame 2 --sprites

Screen coordinates printed are XYZ2 minus XYOFFSET, in whole pixels -- i.e. the
same coordinate space our rasterizer's drawX0/drawY0 use.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gsdump_parse as G

# GIFtag FLG
FLG_PACKED, FLG_REGLIST, FLG_IMAGE, FLG_DISABLE = 0, 1, 2, 3

PRIM_NAMES = {
    0: "point", 1: "line", 2: "linestrip", 3: "tri",
    4: "tristrip", 5: "trifan", 6: "sprite", 7: "invalid",
}

PSM_NAMES = {
    0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S",
    0x13: "T8", 0x14: "T4", 0x1B: "T8H", 0x24: "T4HL", 0x2C: "T4HH",
    0x30: "Z32", 0x31: "Z24", 0x32: "Z16", 0x3A: "Z16S",
}

# A+D register addresses we care about.
REG_PRIM, REG_RGBAQ, REG_ST, REG_UV = 0x00, 0x01, 0x02, 0x03
REG_XYZF2, REG_XYZ2 = 0x04, 0x05
REG_TEX0_1, REG_TEX0_2 = 0x06, 0x07
REG_XYZF3, REG_XYZ3 = 0x0C, 0x0D
REG_XYOFFSET_1, REG_XYOFFSET_2 = 0x18, 0x19
REG_PRMODECONT, REG_PRMODE = 0x1A, 0x1B
REG_TEXA, REG_TEXCLUT = 0x3B, 0x1C
REG_TEST_1, REG_TEST_2 = 0x47, 0x48
REG_ALPHA_1, REG_ALPHA_2 = 0x42, 0x43
REG_FRAME_1, REG_FRAME_2 = 0x4C, 0x4D
REG_BITBLTBUF, REG_TRXPOS, REG_TRXREG, REG_TRXDIR = 0x50, 0x51, 0x52, 0x53


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def decode_tex0(v):
    return {
        "tbp0": bits(v, 0, 14),
        "tbw": bits(v, 14, 6),
        "psm": bits(v, 20, 6),
        "tw": 1 << bits(v, 26, 4),
        "th": 1 << bits(v, 30, 4),
        "tcc": bits(v, 34, 1),
        "tfx": bits(v, 35, 2),
        "cbp": bits(v, 37, 14),
        "cpsm": bits(v, 51, 4),
        "csm": bits(v, 55, 1),
        "csa": bits(v, 56, 5),
        "cld": bits(v, 61, 3),
    }


def tex0_str(t):
    if t is None:
        return "tex0=none"
    return (
        "tbp=0x%04X tbw=%d psm=%s %dx%d tcc=%d tfx=%d "
        "cbp=0x%04X cpsm=%s csm=%d csa=%d cld=%d"
        % (
            t["tbp0"], t["tbw"], PSM_NAMES.get(t["psm"], "0x%02X" % t["psm"]),
            t["tw"], t["th"], t["tcc"], t["tfx"],
            t["cbp"], PSM_NAMES.get(t["cpsm"], "0x%02X" % t["cpsm"]),
            t["csm"], t["csa"], t["cld"],
        )
    )


def decode_prim(v):
    return {
        "type": bits(v, 0, 3),
        "iip": bits(v, 3, 1),
        "tme": bits(v, 4, 1),
        "fge": bits(v, 5, 1),
        "abe": bits(v, 6, 1),
        "aa1": bits(v, 7, 1),
        "fst": bits(v, 8, 1),
        "ctxt": bits(v, 9, 1),
        "fix": bits(v, 10, 1),
    }


class GsState:
    """Just enough GS context to attribute a vertex to a draw."""

    def __init__(self):
        self.prim = decode_prim(0)
        self.prim_raw = 0
        self.tex0 = [None, None]
        self.xyoffset = [(0, 0), (0, 0)]
        self.test = [0, 0]
        self.alpha = [0, 0]
        self.frame = [0, 0]
        self.rgbaq = (0, 0, 0, 0)
        self.q = 1.0
        self.st = (0.0, 0.0)
        self.uv = (0, 0)
        self.vtx = []          # queued vertices for the current primitive
        self.draws = []
        self.frame_index = 0
        self.xfers = []        # BITBLTBUF/TRXREG uploads

    # -- vertex handling ------------------------------------------------
    def push_vertex(self, x, y, z, kick):
        ctx = self.prim["ctxt"]
        ofx, ofy = self.xyoffset[ctx]
        self.vtx.append(
            {
                "x": x, "y": y, "z": z,
                "sx": (x - ofx) / 16.0,
                "sy": (y - ofy) / 16.0,
                "uv": self.uv,
                "st": self.st,
                "q": self.q,
                "rgba": self.rgbaq,
            }
        )
        if not kick:
            self.vtx.pop()  # XYZ3 = no draw kick; keep state, drop the vertex
            return
        need = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}.get(self.prim["type"], 0)
        if need and len(self.vtx) >= need:
            self.emit(self.vtx[-need:])
            # sprites/points/lines/tris consume their vertices; strips/fans do
            # not, but for object identification the consuming model is fine.
            if self.prim["type"] in (0, 1, 3, 6):
                self.vtx = []

    def emit(self, verts):
        ctx = self.prim["ctxt"]
        xs = [v["sx"] for v in verts]
        ys = [v["sy"] for v in verts]
        self.draws.append(
            {
                "frame": self.frame_index,
                "prim": dict(self.prim),
                "prim_raw": self.prim_raw,
                "tex0": self.tex0[ctx],
                "ctx": ctx,
                "x0": min(xs), "y0": min(ys),
                "x1": max(xs), "y1": max(ys),
                "verts": verts,
                "test": self.test[ctx],
                "alpha": self.alpha[ctx],
            }
        )


def apply_reg(st, addr, data):
    """Apply one 64-bit register write."""
    if addr == REG_PRIM:
        st.prim_raw = data & 0x7FF
        st.prim = decode_prim(data)
        st.vtx = []
    elif addr == REG_RGBAQ:
        st.rgbaq = (bits(data, 0, 8), bits(data, 8, 8), bits(data, 16, 8), bits(data, 24, 8))
        st.q = struct.unpack("<f", struct.pack("<I", bits(data, 32, 32)))[0]
    elif addr == REG_ST:
        s = struct.unpack("<f", struct.pack("<I", bits(data, 0, 32)))[0]
        t = struct.unpack("<f", struct.pack("<I", bits(data, 32, 32)))[0]
        st.st = (s, t)
    elif addr == REG_UV:
        st.uv = (bits(data, 0, 14), bits(data, 16, 14))
    elif addr in (REG_XYZ2, REG_XYZF2):
        st.push_vertex(bits(data, 0, 16), bits(data, 16, 16), bits(data, 32, 32), True)
    elif addr in (REG_XYZ3, REG_XYZF3):
        st.push_vertex(bits(data, 0, 16), bits(data, 16, 16), bits(data, 32, 32), False)
    elif addr in (REG_TEX0_1, REG_TEX0_2):
        st.tex0[addr - REG_TEX0_1] = decode_tex0(data)
    elif addr in (REG_XYOFFSET_1, REG_XYOFFSET_2):
        st.xyoffset[addr - REG_XYOFFSET_1] = (bits(data, 0, 16), bits(data, 32, 16))
    elif addr in (REG_TEST_1, REG_TEST_2):
        st.test[addr - REG_TEST_1] = data & 0xFFFFFFFF
    elif addr in (REG_ALPHA_1, REG_ALPHA_2):
        st.alpha[addr - REG_ALPHA_1] = data & 0xFFFFFFFFFFFF
    elif addr in (REG_FRAME_1, REG_FRAME_2):
        st.frame[addr - REG_FRAME_1] = data & 0xFFFFFFFFFFFF
    elif addr == REG_BITBLTBUF:
        st.xfers.append({"bitbltbuf": data, "frame": st.frame_index})
    elif addr in (REG_TRXPOS, REG_TRXREG, REG_TRXDIR) and st.xfers:
        key = {REG_TRXPOS: "trxpos", REG_TRXREG: "trxreg", REG_TRXDIR: "trxdir"}[addr]
        st.xfers[-1][key] = data


def process_packet(st, data):
    """Walk one GIF transfer payload: a chain of GIFtag + register data."""
    pos = 0
    n = len(data)
    while pos + 16 <= n:
        lo = int.from_bytes(data[pos : pos + 8], "little")
        hi = int.from_bytes(data[pos + 8 : pos + 16], "little")
        pos += 16

        nloop = bits(lo, 0, 15)
        eop = bits(lo, 15, 1)
        pre = bits(lo, 46, 1)
        prim = bits(lo, 47, 11)
        flg = bits(lo, 58, 2)
        nreg = bits(lo, 60, 4) or 16

        if pre:
            apply_reg(st, REG_PRIM, prim)

        if flg == FLG_PACKED:
            for _ in range(nloop):
                for i in range(nreg):
                    if pos + 16 > n:
                        return
                    d = int.from_bytes(data[pos : pos + 16], "little")
                    pos += 16
                    reg = bits(hi, i * 4, 4)
                    if reg == 0x0E:  # A+D
                        apply_reg(st, bits(d, 64, 8), d & 0xFFFFFFFFFFFFFFFF)
                    elif reg == 0x0F:  # NOP
                        pass
                    elif reg == REG_XYZ2 or reg == REG_XYZF2:
                        # PACKED XYZ: X bits 0-15, Y bits 32-47, Z bits 64-95
                        st.push_vertex(bits(d, 0, 16), bits(d, 32, 16), bits(d, 64, 32), True)
                    elif reg == REG_XYZ3 or reg == REG_XYZF3:
                        st.push_vertex(bits(d, 0, 16), bits(d, 32, 16), bits(d, 64, 32), False)
                    elif reg == REG_UV:
                        st.uv = (bits(d, 0, 14), bits(d, 32, 14))
                    elif reg == REG_ST:
                        s = struct.unpack("<f", struct.pack("<I", bits(d, 0, 32)))[0]
                        t = struct.unpack("<f", struct.pack("<I", bits(d, 32, 32)))[0]
                        st.st = (s, t)
                        st.q = struct.unpack("<f", struct.pack("<I", bits(d, 64, 32)))[0]
                    elif reg == REG_RGBAQ:
                        st.rgbaq = (bits(d, 0, 8), bits(d, 32, 8), bits(d, 64, 8), bits(d, 96, 8))
                    elif reg == REG_PRIM:
                        apply_reg(st, REG_PRIM, bits(d, 0, 11))
                    else:
                        apply_reg(st, reg, d & 0xFFFFFFFFFFFFFFFF)
        elif flg == FLG_REGLIST:
            total = nloop * nreg
            for i in range(total):
                if pos + 8 > n:
                    return
                d = int.from_bytes(data[pos : pos + 8], "little")
                pos += 8
                apply_reg(st, bits(hi, (i % nreg) * 4, 4), d)
            if total % 2:
                pos += 8  # REGLIST pads to a qword boundary
        elif flg == FLG_IMAGE:
            # Raw texel/CLUT bytes for the transfer set up by the preceding
            # BITBLTBUF/TRXPOS/TRXREG writes. Kept so the actual uploaded
            # image can be read back without re-deriving GS swizzling.
            blob = data[pos : pos + nloop * 16]
            pos += nloop * 16
            if st.xfers:
                st.xfers[-1].setdefault("image", bytearray()).extend(blob)
        else:
            pass  # FLG_DISABLE

        if eop and pos >= n:
            break


def hx(v):
    return "0x%X" % v


def draw_to_jsonl_record(seq, d):
    """Match the field names GS::dumpDebugHistoryJsonl() emits on the live side,
    so the two streams can be diffed key-for-key by gsdump_diff.py."""
    t = d["tex0"] or {}
    p = d["prim"]
    return {
        "probe": "GSDRAW",
        "seq": hx(seq),
        "frame": hx(d["frame"]),
        "prim": hx(p["type"]),
        "tme": hx(p["tme"]),
        "abe": hx(p["abe"]),
        "tbp0": hx(t.get("tbp0", 0)),
        "tbw": hx(t.get("tbw", 0)),
        "psm": hx(t.get("psm", 0)),
        "tw": hx(t.get("tw", 0)),
        "th": hx(t.get("th", 0)),
        "cbp": hx(t.get("cbp", 0)),
        "cpsm": hx(t.get("cpsm", 0)),
        "csm": hx(t.get("csm", 0)),
        "csa": hx(t.get("csa", 0)),
        "cld": hx(t.get("cld", 0)),
        "test": hx(d["test"]),
        "alpha": hx(d["alpha"]),
        "x0": "%.1f" % d["x0"], "y0": "%.1f" % d["y0"],
        "x1": "%.1f" % d["x1"], "y1": "%.1f" % d["y1"],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--sprites", action="store_true", help="list sprite draws only")
    ap.add_argument("--all", action="store_true", help="list every draw")
    ap.add_argument("--rect", nargs=4, type=float, metavar=("X0", "Y0", "X1", "Y1"),
                    help="only draws overlapping this screen rect")
    ap.add_argument("--frame", type=int, help="only this frame index")
    ap.add_argument("--min-area", type=float, default=0.0, help="skip draws smaller than this")
    ap.add_argument("--uploads", action="store_true", help="list BITBLTBUF uploads instead")
    ap.add_argument("--limit", type=int, default=80)
    ap.add_argument("--emit-jsonl", type=Path, metavar="PATH",
                    help="write one JSON object per draw (dump-side ground truth) "
                         "for gsdump_diff.py; use '-' for stdout. Honors --frame/"
                         "--sprites/--rect/--min-area filters like normal output")
    args = ap.parse_args()

    raw = G.decompress(args.dump, args.dump.read_bytes())
    info, state, regs, reader = G.parse_container(raw)
    packets = G.parse_packets(reader)

    st = GsState()
    for p in packets:
        if p["id"] == G.PACKET_TRANSFER:
            process_packet(st, p["data"])
        elif p["id"] == G.PACKET_VSYNC:
            st.frame_index += 1

    print("serial=%s crc=0x%08X packets=%d draws=%d frames=%d"
          % (info.get("serial"), info.get("crc", 0), len(packets), len(st.draws), st.frame_index + 1))

    if args.uploads:
        print("\n== uploads (BITBLTBUF) ==")
        for x in st.xfers[: args.limit]:
            b = x["bitbltbuf"]
            r = x.get("trxreg", 0)
            print("  f%d dbp=0x%04X dbw=%d dpsm=%-5s  %dx%d  sbp=0x%04X spsm=%s dir=%s"
                  % (x["frame"], bits(b, 32, 14), bits(b, 48, 6),
                     PSM_NAMES.get(bits(b, 56, 6), "0x%02X" % bits(b, 56, 6)),
                     bits(r, 0, 12), bits(r, 32, 12),
                     bits(b, 0, 14), PSM_NAMES.get(bits(b, 24, 6), "0x%02X" % bits(b, 24, 6)),
                     x.get("trxdir", "?")))
        return

    sel = st.draws
    if args.frame is not None:
        sel = [d for d in sel if d["frame"] == args.frame]
    if args.sprites:
        sel = [d for d in sel if d["prim"]["type"] == 6]
    if args.rect:
        rx0, ry0, rx1, ry1 = args.rect
        sel = [d for d in sel if not (d["x1"] < rx0 or d["x0"] > rx1 or d["y1"] < ry0 or d["y0"] > ry1)]
    if args.min_area:
        sel = [d for d in sel if (d["x1"] - d["x0"]) * (d["y1"] - d["y0"]) >= args.min_area]

    if args.emit_jsonl:
        lines = [json.dumps(draw_to_jsonl_record(i, d)) for i, d in enumerate(sel)]
        text = "\n".join(lines) + ("\n" if lines else "")
        if str(args.emit_jsonl) == "-":
            sys.stdout.write(text)
        else:
            args.emit_jsonl.write_text(text)
            print("wrote %d draws to %s" % (len(sel), args.emit_jsonl), file=sys.stderr)
        return

    print("matched %d draws (showing %d)\n" % (len(sel), min(len(sel), args.limit)))
    for i, d in enumerate(sel[: args.limit]):
        p = d["prim"]
        w = d["x1"] - d["x0"]
        h = d["y1"] - d["y0"]
        print("[%3d] f%d %-9s prim=0x%03X tme=%d abe=%d fst=%d iip=%d ctx=%d  "
              "rect=(%.1f,%.1f)-(%.1f,%.1f) %.0fx%.0f"
              % (i, d["frame"], PRIM_NAMES.get(p["type"], "?"), d["prim_raw"],
                 p["tme"], p["abe"], p["fst"], p["iip"], d["ctx"],
                 d["x0"], d["y0"], d["x1"], d["y1"], w, h))
        print("      %s" % tex0_str(d["tex0"]))
        for v in d["verts"]:
            print("      vtx sx=%8.2f sy=%8.2f  uv=(%d,%d)  st=(%.6f,%.6f) q=%.6f  rgba=%s"
                  % (v["sx"], v["sy"], v["uv"][0], v["uv"][1],
                     v["st"][0], v["st"][1], v["q"], v["rgba"]))


if __name__ == "__main__":
    main()
