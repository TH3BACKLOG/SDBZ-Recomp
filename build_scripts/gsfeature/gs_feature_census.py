#!/usr/bin/env python3
"""Step 3 of the feature-not-scene plan: which GS features does SDBZ actually draw with?

Walks every GS capture we already have (our .gsr and PCSX2 .gs/.gs.zst) and counts, per
draw kick, the GS state that the rasterizer branches on. No run, no build.

    python gs_feature_census.py                       # every gsdump/* dir, 4 workers
    python gs_feature_census.py --dirs tour vsall     # some dirs only
    python gs_feature_census.py --every 4             # 1 file in 4 per dir (fast)
    python gs_feature_census.py --json census.json

Output: one table per feature (value -> draw count, file count, first file) and a
WATCH list for the ps2x_tests features that fail today (T4HL/T4HH, PABE, TEX2,
bilinear on T4 STQ, triangle fans).
Counts are per vertex kick for strips/fans (gsdump_draws.GsState model), so read them
as "used / not used", not as primitive counts.
"""
import argparse
import collections
import json
import multiprocessing
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import gsdump_draws as D  # noqa: E402
import gsdump_parse as G  # noqa: E402

ROOT = HERE.parent.parent / "gsdump"

PSM = {0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S", 0x13: "T8", 0x14: "T4",
       0x1B: "T8H", 0x24: "T4HL", 0x2C: "T4HH", 0x30: "Z32", 0x31: "Z24", 0x32: "Z16", 0x3A: "Z16S"}
PRIM = {0: "point", 1: "line", 2: "linestrip", 3: "tri", 4: "tristrip", 5: "trifan", 6: "sprite", 7: "bad7"}
ABCD = ("Cs", "Cd", "0", "?")
CABC = ("As", "Ad", "FIX", "?")
ATST = ("NEVER", "ALWAYS", "LESS", "LEQUAL", "EQUAL", "GEQUAL", "GREATER", "NOTEQUAL")
AFAIL = ("KEEP", "FB_ONLY", "ZB_ONLY", "RGB_ONLY")
ZTST = ("NEVER", "ALWAYS", "GEQUAL", "GREATER")
TFX = ("MODULATE", "DECAL", "HIGHLIGHT", "HIGHLIGHT2")
FILT = ("NEAREST", "LINEAR", "N_MIP_N", "N_MIP_L", "L_MIP_N", "L_MIP_L", "?6", "?7")

R_TEX1 = (0x14, 0x15)
R_TEX2 = (0x16, 0x17)
R_CLAMP = (0x08, 0x09)
R_ZBUF = (0x4E, 0x4F)
R_FBA = (0x4A, 0x4B)
R_FRAME = (0x4C, 0x4D)
R_SCANMSK, R_TEXA, R_FOGCOL, R_DTHE, R_COLCLAMP, R_PABE = 0x22, 0x3B, 0x3D, 0x45, 0x46, 0x49


def b(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


class CensusState(D.GsState):
    """GsState that counts features per draw instead of keeping the draw list."""

    def __init__(self, counts, tag):
        super().__init__()
        self.c = counts
        self.tag = tag
        self.tex1 = [0, 0]
        self.clamp = [0, 0]
        self.zbuf = [0, 0]
        self.fba = [0, 0]
        self.frame2 = [0, 0]
        self.pabe = self.texa = self.dthe = self.scanmsk = 0
        self.colclamp = 1

    def hit(self, feat, val):
        self.c[(feat, val)] += 1

    def emit(self, verts):
        p, ctx = self.prim, self.prim["ctxt"]
        self.hit("prim", PRIM[p["type"]])
        for k in ("iip", "tme", "fge", "abe", "aa1", "fst"):
            if p[k]:
                self.hit("prim." + k, 1)
        fpsm = b(self.frame[ctx], 24, 6)
        self.hit("frame.psm", PSM.get(fpsm, hex(fpsm)))
        fb_alpha = fpsm in (0x00, 0x02, 0x0A)
        if p["tme"]:
            t = self.tex0[ctx] or D.decode_tex0(0)
            psm = PSM.get(t["psm"], hex(t["psm"]))
            self.hit("tex.psm", psm)
            self.hit("tex.tfx/tcc", "%s/tcc%d" % (TFX[t["tfx"]], t["tcc"]))
            if t["psm"] in (0x13, 0x14, 0x1B, 0x24, 0x2C):
                self.hit("clut", "%s cpsm=%s csm%d" % (psm, PSM.get(t["cpsm"], hex(t["cpsm"])), t["csm"] + 1))
            if t["psm"] in (0x01, 0x02, 0x0A) or (t["psm"] in (0x13, 0x14, 0x1B, 0x24, 0x2C) and t["cpsm"] != 0):
                self.hit("texa(aem/ta0/ta1)", "aem%d ta0=%d ta1=%d" % (b(self.texa, 15, 1), b(self.texa, 0, 8), b(self.texa, 32, 8)))
            t1 = self.tex1[ctx]
            mag, mn = FILT[b(t1, 5, 1)], FILT[b(t1, 6, 3)]
            self.hit("tex1.filter", "mag=%s min=%s lcm=%d mxl=%d" % (mag, mn, b(t1, 0, 1), b(t1, 2, 3)))
            if mag != "NEAREST" or mn != "NEAREST":
                self.hit("WATCH bilinear", "%s %s" % (psm, "UV" if p["fst"] else "STQ"))
            cl = self.clamp[ctx]
            self.hit("clamp", "wms=%d wmt=%d" % (b(cl, 0, 2), b(cl, 2, 2)))
            if t["psm"] in (0x24, 0x2C):
                self.hit("WATCH T4HL/T4HH", psm)
        if p["abe"]:
            a = self.alpha[ctx]
            self.hit("alpha", "(%s-%s)*%s+%s" % (ABCD[b(a, 0, 2)], ABCD[b(a, 2, 2)], CABC[b(a, 4, 2)], ABCD[b(a, 6, 2)]))
            if b(a, 4, 2) == 2:
                self.hit("alpha.fix", b(a, 32, 8))
            if self.pabe:
                self.hit("WATCH PABE", 1)
            self.hit("colclamp", self.colclamp & 1)
        if fb_alpha and self.fba[ctx] & 1:
            self.hit("fba", 1)
        if self.dthe & 1:
            self.hit("dthe", 1)
        if self.scanmsk & 3:
            self.hit("scanmsk", self.scanmsk & 3)
        if p["fge"]:
            self.hit("fog", 1)
        tst = self.test[ctx]
        if b(tst, 0, 1):
            self.hit("test.alpha", "%s ref=%d afail=%s" % (ATST[b(tst, 1, 3)], b(tst, 4, 8), AFAIL[b(tst, 12, 2)]))
        if b(tst, 14, 1):
            self.hit("test.date", "datm%d" % b(tst, 15, 1))
        if b(tst, 16, 1):
            self.hit("test.z", "%s zmsk=%d zpsm=%s" % (ZTST[b(tst, 17, 2)], b(self.zbuf[ctx], 32, 1),
                                                     PSM.get(0x30 | b(self.zbuf[ctx], 24, 4), "?")))
        if p["type"] == 5:
            self.hit("WATCH trifan", "tme%d" % p["tme"])


def track(st, addr, data):
    """Registers gsdump_draws.apply_reg ignores."""
    if addr in R_TEX1:
        st.tex1[addr - R_TEX1[0]] = data
    elif addr in R_TEX2:
        st.hit("WATCH TEX2 write", "ctx%d" % (addr - R_TEX2[0] + 1))
        ctx = addr - R_TEX2[0]
        old = st.tex0[ctx] or D.decode_tex0(0)
        new = D.decode_tex0(data)
        for k in ("psm", "cbp", "cpsm", "csm", "csa", "cld"):
            old[k] = new[k]
        st.tex0[ctx] = old
    elif addr in R_CLAMP:
        st.clamp[addr - R_CLAMP[0]] = data
    elif addr in R_ZBUF:
        st.zbuf[addr - R_ZBUF[0]] = data
    elif addr in R_FBA:
        st.fba[addr - R_FBA[0]] = data
    elif addr == R_PABE:
        st.pabe = data & 1
    elif addr == R_TEXA:
        st.texa = data
    elif addr == R_DTHE:
        st.dthe = data
    elif addr == R_SCANMSK:
        st.scanmsk = data
    elif addr == R_COLCLAMP:
        st.colclamp = data


_orig_apply = D.apply_reg


def apply_reg(st, addr, data):
    _orig_apply(st, addr, data)
    if isinstance(st, CensusState):
        track(st, addr, data)


D.apply_reg = apply_reg  # process_packet looks the name up at call time


def packed_qword(st, reg, d):
    """One PACKED-mode qword, as gsdump_draws.process_packet decodes it."""
    if reg == 0x0E:  # A+D
        apply_reg(st, b(d, 64, 8), d & 0xFFFFFFFFFFFFFFFF)
    elif reg == 0x0F:
        pass
    elif reg in (D.REG_XYZ2, D.REG_XYZF2):
        st.push_vertex(b(d, 0, 16), b(d, 32, 16), b(d, 64, 32), True)
    elif reg in (D.REG_XYZ3, D.REG_XYZF3):
        st.push_vertex(b(d, 0, 16), b(d, 32, 16), b(d, 64, 32), False)
    elif reg == D.REG_UV:
        st.uv = (b(d, 0, 14), b(d, 32, 14))
    elif reg in (D.REG_ST, D.REG_RGBAQ):
        pass  # vertex colour / coords do not change the feature key
    elif reg == D.REG_PRIM:
        apply_reg(st, D.REG_PRIM, b(d, 0, 11))
    else:
        apply_reg(st, reg, d & 0xFFFFFFFFFFFFFFFF)


class GifPath:
    """Resumable GIF parser for one PATH. A GIF packet may span several dump transfers
    (gsdump_draws.process_packet assumes each transfer starts with a tag, which turns
    image data into fake register writes: the invalid PSMs of the first census)."""

    def __init__(self, st):
        self.st = st
        self.buf = bytearray()
        self.left = 0      # register slots (PACKED/REGLIST) or qwords (IMAGE) left in the tag
        self.flg = 0
        self.regs = 0
        self.nreg = 0
        self.i = 0         # slot index inside the current tag

    def feed(self, data):
        self.buf += data
        pos, n, buf = 0, len(self.buf), self.buf
        while True:
            if self.left == 0:
                if pos + 16 > n:
                    break
                lo, hi = int.from_bytes(buf[pos:pos + 8], "little"), int.from_bytes(buf[pos + 8:pos + 16], "little")
                pos += 16
                nloop, self.flg, self.nreg = b(lo, 0, 15), b(lo, 58, 2), (b(lo, 60, 4) or 16)
                self.regs, self.i = hi, 0
                if b(lo, 46, 1):
                    apply_reg(self.st, D.REG_PRIM, b(lo, 47, 11))
                if self.flg == 2 or self.flg == 3:
                    self.left = nloop
                else:
                    self.left = nloop * self.nreg
                    if self.flg == 1 and self.left % 2:
                        self.left += 1  # REGLIST pads to a qword: one dummy slot
                        self.pad = True
                    else:
                        self.pad = False
                continue
            if self.flg == 0:
                if pos + 16 > n:
                    break
                d = int.from_bytes(buf[pos:pos + 16], "little")
                pos += 16
                packed_qword(self.st, b(self.regs, (self.i % self.nreg) * 4, 4), d)
            elif self.flg == 1:
                if pos + 8 > n:
                    break
                d = int.from_bytes(buf[pos:pos + 8], "little")
                pos += 8
                if not (self.pad and self.left == 1):
                    apply_reg(self.st, b(self.regs, (self.i % self.nreg) * 4, 4), d)
            else:  # IMAGE / DISABLE: skip whole qwords
                take = min(self.left, (n - pos) // 16)
                if take == 0:
                    break
                pos += take * 16
                self.left -= take
                continue
            self.i += 1
            self.left -= 1
        del self.buf[:pos]


def census_file(path):
    counts = collections.Counter()
    st = CensusState(counts, path)
    paths = collections.defaultdict(lambda: GifPath(st))
    try:
        if path.endswith(".gsr"):
            regs, xfers = G.read_gsr(path)
            for p, data in xfers:
                paths[p].feed(data)
        else:
            raw = G.decompress(Path(path), Path(path).read_bytes())
            _info, _state, _regs, reader = G.parse_container(raw)
            for pk in G.parse_packets(reader):
                if pk["id"] == G.PACKET_TRANSFER:
                    paths[pk["path"]].feed(pk["data"])
    except (Exception, SystemExit) as e:  # one bad file must not kill the census
        counts[("ERROR", type(e).__name__)] += 1
    return path, counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dirs", nargs="*", help="gsdump sub-dirs (default: all)")
    ap.add_argument("--every", type=int, default=1, help="take 1 file in N per dir")
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()

    dirs = [ROOT / d for d in a.dirs] if a.dirs else sorted(p for p in ROOT.iterdir() if p.is_dir())
    files = []
    for d in dirs:
        fs = sorted(str(f) for f in d.iterdir()
                    if f.name.endswith((".gsr", ".gs", ".gs.zst", ".gs.xz")))
        files += fs[::a.every]
    print("census: %d files in %d dirs, %d workers" % (len(files), len(dirs), a.workers), file=sys.stderr)

    total = collections.Counter()
    nfiles = collections.Counter()
    first = {}
    with multiprocessing.Pool(a.workers) as pool:
        for i, (path, c) in enumerate(pool.imap_unordered(census_file, files)):
            total.update(c)
            for k in c:
                nfiles[k] += 1
                first.setdefault(k, os.path.relpath(path, ROOT))
            if (i + 1) % 50 == 0:
                print("  %d/%d" % (i + 1, len(files)), file=sys.stderr)

    feats = collections.defaultdict(list)
    for (f, v), n in total.items():
        feats[f].append((v, n, nfiles[(f, v)], first[(f, v)]))
    order = sorted(feats, key=lambda f: (f.startswith("WATCH") is False, f))
    for f in order:
        print("\n## %s" % f)
        for v, n, nf, fp in sorted(feats[f], key=lambda r: -r[1]):
            print("  %-44s draws=%-9d files=%-4d first=%s" % (v, n, nf, fp))
    watch = ("WATCH T4HL/T4HH", "WATCH PABE", "WATCH TEX2 write", "WATCH bilinear", "WATCH trifan")
    print("\n## WATCH summary (failing ps2x_tests features)")
    for w in watch:
        print("  %-20s %s" % (w, "USED" if w in feats else "not seen"))
    if a.json:
        a.json.write_text(json.dumps({f"{f}|{v}": [n, nfiles[(f, v)], first[(f, v)]]
                                      for (f, v), n in total.items()}, indent=1))


if __name__ == "__main__":
    main()
