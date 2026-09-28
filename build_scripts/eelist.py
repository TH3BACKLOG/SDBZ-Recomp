#!/usr/bin/env python3
"""Walk an EE DMA source-chain display list and decode it into GS draws.

Why this exists: Stage 5.11 needs the *guest-side* draw order with the EE address
of each packet, so PCSX2's list can be compared against our runtime's [drawpath]
`src` column entry-for-entry. The skill's vif_gif_surgeon.py decodes exactly one
DMA tag and cannot follow a chain, so it cannot answer ordering questions at all.

Output is JSONL using the same GSDRAW schema as live.jsonl / dump.jsonl
(probe, seq, frame, prim, tme, abe, tbp0, tbw, psm, tw, th, cbp, cpsm, csm, csa,
cld, test, alpha, x0, y0, x1, y1) so it drops straight into gsdump_diff.py and
gsdump_draws.py, plus two extra fields:

    src   EE address of the GIFtag that produced the draw  (the join key)
    tag   EE address of the DMA tag whose payload contained it

Texture/CLUT uploads are emitted as GSXFER rows in list order, because *when* a
palette lands relative to the draw that samples it is the other half of 5.11.

Coordinates are raw GS pixels including XYOFFSET, matching live.jsonl.
Use --xyoffset to subtract an origin (or 'auto' for the modal minimum).

Usage:
    python eelist.py buf.bin --base 0x770000 --start 0x771400
    python eelist.py buf.bin --base 0x770000 --start 0x771400 --jsonl out.jsonl
"""

import argparse
import json
import struct
import sys
from collections import Counter

DMA_IDS = {0: "REFE", 1: "CNT", 2: "NEXT", 3: "REF", 4: "REFS", 5: "CALL", 6: "RET", 7: "END"}

# GS register addresses used as A+D targets / PACKED REGS nibbles.
GS_PRIM, GS_RGBAQ, GS_ST, GS_UV, GS_XYZF2, GS_XYZ2 = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05
GS_TEX0_1, GS_TEX0_2 = 0x06, 0x07
GS_XYZF3, GS_XYZ3, GS_AD, GS_NOP = 0x0C, 0x0D, 0x0E, 0x0F
GS_XYOFFSET_1, GS_XYOFFSET_2 = 0x18, 0x19
GS_ALPHA_1, GS_ALPHA_2 = 0x42, 0x43
GS_TEST_1, GS_TEST_2 = 0x47, 0x48
GS_FRAME_1, GS_FRAME_2 = 0x4C, 0x4D
GS_BITBLTBUF, GS_TRXPOS, GS_TRXREG, GS_TRXDIR = 0x50, 0x51, 0x52, 0x53

# Vertex-kick registers: writing one emits a vertex.
KICK_REGS = {GS_XYZ2, GS_XYZF2, GS_XYZ3, GS_XYZF3}
# XYZ3/XYZF3 set the vertex without kicking the drawing primitive.
NO_DRAW_KICK = {GS_XYZ3, GS_XYZF3}

PRIM_VERTS = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}  # point/line/linestrip/tri/strip/fan/sprite


def bits(value, lo, hi):
    return (value >> lo) & ((1 << (hi - lo + 1)) - 1)


class Tex0:
    __slots__ = ("tbp0", "tbw", "psm", "tw", "th", "tcc", "tfx",
                 "cbp", "cpsm", "csm", "csa", "cld")

    def __init__(self, raw=0):
        self.tbp0 = bits(raw, 0, 13)
        self.tbw = bits(raw, 14, 19)
        self.psm = bits(raw, 20, 25)
        self.tw = 1 << bits(raw, 26, 29)
        self.th = 1 << bits(raw, 30, 33)
        self.tcc = bits(raw, 34, 34)
        self.tfx = bits(raw, 35, 36)
        self.cbp = bits(raw, 37, 50)
        self.cpsm = bits(raw, 51, 54)
        self.csm = bits(raw, 55, 55)
        self.csa = bits(raw, 56, 60)
        self.cld = bits(raw, 61, 63)


class Decoder:
    def __init__(self, data, base, verbose=False):
        self.data = data
        self.base = base
        self.verbose = verbose
        self.rows = []
        self.seq = 0
        self.skipped = 0

        # GS context state, per drawing context (ctxt 0/1).
        self.tex0 = [Tex0(), Tex0()]
        self.test = [0, 0]
        self.alpha = [0, 0]
        self.xyoffset = [(0, 0), (0, 0)]
        self.frame = [0, 0]
        self.prim = 0
        self.verts = []
        # Pending transfer state, latched until TRXDIR kicks it.
        self.bitbltbuf = 0
        self.trxreg = 0
        self.trxpos = 0

    # ---- raw access -------------------------------------------------------
    def qword(self, addr):
        off = addr - self.base
        if off < 0 or off + 16 > len(self.data):
            raise IndexError(f"0x{addr:08X} outside dumped range")
        lo, hi = struct.unpack_from("<QQ", self.data, off)
        return lo, hi

    def u64(self, addr):
        off = addr - self.base
        if off < 0 or off + 8 > len(self.data):
            raise IndexError(f"0x{addr:08X} outside dumped range")
        return struct.unpack_from("<Q", self.data, off)[0]

    # ---- DMA chain --------------------------------------------------------
    def walk(self, start, max_tags=20000):
        """Follow the DMA source chain from `start`, decoding each GIF payload."""
        addr = start
        stack = []
        tags = 0

        while tags < max_tags:
            tags += 1
            lo, hi = self.qword(addr)
            qwc = lo & 0xFFFF
            tag_id = (lo >> 28) & 0x7
            next_addr = (lo >> 32) & 0xFFFFFFFF
            vif0 = hi & 0xFFFFFFFF
            vif1 = (hi >> 32) & 0xFFFFFFFF

            if self.verbose:
                print(f"  tag 0x{addr:08X} {DMA_IDS[tag_id]:<4} qwc={qwc:<4} "
                      f"addr=0x{next_addr:08X} vif0=0x{vif0:08X} vif1=0x{vif1:08X}",
                      file=sys.stderr)

            # Where does this tag's payload live, and where is the next tag?
            if tag_id in (1, 2, 7):            # CNT / NEXT / END: data follows the tag
                data_at = addr + 16
                after = data_at + qwc * 16
                follow = after if tag_id == 1 else next_addr
            elif tag_id in (0, 3, 4):          # REFE / REF / REFS: data is elsewhere
                data_at = next_addr
                follow = addr + 16
            elif tag_id == 5:                  # CALL
                data_at = addr + 16
                stack.append(data_at + qwc * 16)
                follow = next_addr
            else:                              # RET
                data_at = addr + 16
                follow = stack.pop() if stack else None

            if qwc:
                self.decode_vif_payload(data_at, qwc, vif0, vif1, addr)

            if tag_id in (0, 7):               # REFE and END terminate the chain
                break
            if follow is None:
                break
            addr = follow

        return tags

    def decode_vif_payload(self, addr, qwc, vif0, vif1, tag_addr):
        """The tag's VIF codes decide what the payload is. We only care about GIF."""
        direct = False
        for code in (vif0, vif1):
            cmd = (code >> 24) & 0x7F
            if cmd in (0x50, 0x51):            # DIRECT / DIRECTHL
                direct = True
        if not direct:
            return                             # VU microcode / unpack — not a GS draw
        try:
            self.decode_gif(addr, addr + qwc * 16, tag_addr)
        except IndexError:
            # REF tags routinely point at texture image data living outside the
            # dumped window. That payload carries no draws, so skipping it is
            # correct — but count it, so a truncated dump cannot masquerade as a
            # short display list.
            self.skipped += 1

    # ---- GIF --------------------------------------------------------------
    def decode_gif(self, addr, end, tag_addr):
        while addr + 16 <= end:
            gif_addr = addr
            lo, hi = self.qword(addr)
            addr += 16

            nloop = lo & 0x7FFF
            eop = (lo >> 15) & 1
            pre = (lo >> 46) & 1
            prim = (lo >> 47) & 0x7FF
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16

            if pre:
                self.set_prim(prim)

            if flg == 0:                       # PACKED
                for _ in range(nloop):
                    for r in range(nreg):
                        if addr + 16 > end:
                            return
                        d_lo, d_hi = self.qword(addr)
                        addr += 16
                        reg = (hi >> (4 * r)) & 0xF
                        if reg == GS_AD:
                            self.write_reg(d_hi & 0xFF, d_lo, gif_addr, tag_addr)
                        else:
                            self.packed_reg(reg, d_lo, d_hi, gif_addr, tag_addr)
            elif flg == 1:                     # REGLIST
                total = nloop * nreg
                for i in range(total):
                    if addr + 8 > end:
                        return
                    value = self.u64(addr)
                    addr += 8
                    reg = (hi >> (4 * (i % nreg))) & 0xF
                    self.write_reg(reg, value, gif_addr, tag_addr)
                if total & 1:                  # REGLIST pads to a qword boundary
                    addr += 8
            else:                              # IMAGE / disabled
                addr += nloop * 16

            # EOP ends *this* GIF packet, not the DMA payload. These lists put
            # PRIM in a 1-reg A+D packet with EOP set and the vertex REGLIST in
            # the very next GIFtag, so stopping on EOP drops every sprite.
            _ = eop

    def packed_reg(self, reg, d_lo, d_hi, gif_addr, tag_addr):
        """PACKED mode stores several regs in a non-native layout."""
        if reg in (GS_XYZ2, GS_XYZ3):
            x = d_lo & 0xFFFF
            y = (d_lo >> 32) & 0xFFFF
            z = d_hi & 0xFFFFFFFF
            self.write_reg(reg, x | (y << 16) | (z << 32), gif_addr, tag_addr)
        elif reg == GS_PRIM:
            self.set_prim(d_lo & 0x7FF)
        else:
            self.write_reg(reg, d_lo, gif_addr, tag_addr)

    def set_prim(self, prim):
        self.prim = prim
        self.verts = []                        # a new PRIM restarts vertex assembly

    # ---- GS register writes ----------------------------------------------
    def write_reg(self, reg, value, gif_addr, tag_addr):
        if reg == GS_PRIM:
            self.set_prim(value & 0x7FF)
        elif reg in (GS_TEX0_1, GS_TEX0_2):
            self.tex0[reg - GS_TEX0_1] = Tex0(value)
        elif reg in (GS_TEST_1, GS_TEST_2):
            self.test[reg - GS_TEST_1] = value
        elif reg in (GS_ALPHA_1, GS_ALPHA_2):
            self.alpha[reg - GS_ALPHA_1] = value
        elif reg in (GS_XYOFFSET_1, GS_XYOFFSET_2):
            self.xyoffset[reg - GS_XYOFFSET_1] = (bits(value, 0, 15), bits(value, 32, 47))
        elif reg in (GS_FRAME_1, GS_FRAME_2):
            self.frame[reg - GS_FRAME_1] = value
        elif reg == GS_BITBLTBUF:
            self.bitbltbuf = value
        elif reg == GS_TRXPOS:
            self.trxpos = value
        elif reg == GS_TRXREG:
            self.trxreg = value
        elif reg == GS_TRXDIR:
            self.emit_xfer(value, gif_addr, tag_addr)
        elif reg in KICK_REGS:
            self.kick(reg, value, gif_addr, tag_addr)

    def kick(self, reg, value, gif_addr, tag_addr):
        x = bits(value, 0, 15) / 16.0
        y = bits(value, 16, 31) / 16.0
        self.verts.append((x, y))
        if reg in NO_DRAW_KICK:
            self.verts.pop()
            return
        need = PRIM_VERTS.get(self.prim & 0x7, 2)
        if len(self.verts) >= need:
            self.emit_draw(self.verts[-need:], gif_addr, tag_addr)
            # Sprites and triangles consume their vertices; strips/fans keep sliding.
            if (self.prim & 0x7) in (0, 1, 3, 6):
                self.verts = []

    # ---- emit -------------------------------------------------------------
    def emit_draw(self, verts, gif_addr, tag_addr):
        ctxt = (self.prim >> 9) & 1
        t = self.tex0[ctxt]
        tme = (self.prim >> 4) & 1
        xs = [v[0] for v in verts]
        ys = [v[1] for v in verts]

        self.rows.append({
            "probe": "GSDRAW",
            "seq": hex(self.seq),
            "frame": "0x0",
            "prim": hex(self.prim & 0x7),
            "tme": hex(tme),
            "abe": hex((self.prim >> 6) & 1),
            "tbp0": hex(t.tbp0 if tme else 0).upper().replace("0X", "0x"),
            "tbw": hex(t.tbw if tme else 0),
            "psm": hex(t.psm if tme else 0).upper().replace("0X", "0x"),
            "tw": hex(t.tw if tme else 1),
            "th": hex(t.th if tme else 1),
            "cbp": hex(t.cbp if tme else 0).upper().replace("0X", "0x"),
            "cpsm": hex(t.cpsm if tme else 0),
            "csm": hex(t.csm if tme else 0),
            "csa": hex(t.csa if tme else 0),
            "cld": hex(t.cld if tme else 0),
            "test": hex(self.test[ctxt]).upper().replace("0X", "0x"),
            "alpha": hex(self.alpha[ctxt]).upper().replace("0X", "0x"),
            "x0": f"{min(xs)}", "y0": f"{min(ys)}",
            "x1": f"{max(xs)}", "y1": f"{max(ys)}",
            "src": f"0x{gif_addr:08X}",
            "tag": f"0x{tag_addr:08X}",
        })
        self.seq += 1

    def emit_xfer(self, trxdir, gif_addr, tag_addr):
        xdir = trxdir & 3
        if xdir == 3:
            return
        self.rows.append({
            "probe": "GSXFER",
            "seq": hex(self.seq),
            "dir": ("host->local", "local->host", "local->local")[xdir],
            "dbp": f"0x{bits(self.bitbltbuf, 32, 45):X}",
            "dbw": hex(bits(self.bitbltbuf, 48, 53)),
            "dpsm": f"0x{bits(self.bitbltbuf, 56, 61):X}",
            "sbp": f"0x{bits(self.bitbltbuf, 0, 13):X}",
            "spsm": f"0x{bits(self.bitbltbuf, 24, 29):X}",
            "w": str(bits(self.trxreg, 0, 11)),
            "h": str(bits(self.trxreg, 32, 43)),
            "dsax": str(bits(self.trxpos, 32, 42)),
            "dsay": str(bits(self.trxpos, 48, 58)),
            "src": f"0x{gif_addr:08X}",
            "tag": f"0x{tag_addr:08X}",
        })
        self.seq += 1


def apply_xyoffset(rows, mode):
    draws = [r for r in rows if r["probe"] == "GSDRAW"]
    if not draws or mode == "none":
        return
    if mode == "auto":
        ox = Counter(float(r["x0"]) for r in draws).most_common(1)[0][0]
        oy = Counter(float(r["y0"]) for r in draws).most_common(1)[0][0]
    else:
        ox, oy = (float(v) for v in mode.split(","))
    for r in draws:
        for k, o in (("x0", ox), ("x1", ox), ("y0", oy), ("y1", oy)):
            r[k] = f"{float(r[k]) - o}"


def summarize(rows):
    print(f"{'#':>4}  {'src':<10} {'kind':<6} {'what':<46} detail")
    print("-" * 108)
    for i, r in enumerate(rows):
        if r["probe"] == "GSXFER":
            what = f"UPLOAD {r['w']}x{r['h']} psm={r['dpsm']} -> dbp={r['dbp']}"
            detail = f"dbw={r['dbw']} at({r['dsax']},{r['dsay']}) {r['dir']}"
        else:
            w = float(r["x1"]) - float(r["x0"])
            h = float(r["y1"]) - float(r["y0"])
            if r["tme"] == "0x1":
                what = f"draw {w:g}x{h:g} @({r['x0']},{r['y0']}) tbp0={r['tbp0']}"
                detail = (f"psm={r['psm']} tbw={r['tbw']} {r['tw']}x{r['th']} "
                          f"cbp={r['cbp']} cld={r['cld']} alpha={r['alpha']}")
            else:
                what = f"draw {w:g}x{h:g} @({r['x0']},{r['y0']}) UNTEXTURED"
                detail = f"alpha={r['alpha']} test={r['test']}"
        print(f"{i:>4}  {r['src'][2:]:<10} {r['probe'][2:]:<6} {what:<46} {detail}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary", help="raw EE memory dump (see pcsx2_ee.py --dump)")
    ap.add_argument("--base", required=True, help="EE address the dump starts at")
    ap.add_argument("--start", required=True, help="EE address of the first DMA tag")
    ap.add_argument("--xyoffset", default="none",
                    help="'auto', 'X,Y', or 'none' (default) to keep raw GS pixels")
    ap.add_argument("--jsonl", help="write GSDRAW/GSXFER rows here")
    ap.add_argument("--verbose", action="store_true", help="trace DMA tags to stderr")
    args = ap.parse_args()

    with open(args.binary, "rb") as fh:
        data = fh.read()

    dec = Decoder(data, int(args.base, 0), args.verbose)
    try:
        tags = dec.walk(int(args.start, 0))
    except IndexError as exc:
        print(f"chain left the dumped range: {exc}", file=sys.stderr)
        tags = -1

    apply_xyoffset(dec.rows, args.xyoffset)
    summarize(dec.rows)

    draws = sum(1 for r in dec.rows if r["probe"] == "GSDRAW")
    xfers = len(dec.rows) - draws
    print(f"\n{tags} DMA tags, {draws} draws, {xfers} transfers")

    if args.jsonl:
        with open(args.jsonl, "w", encoding="utf-8") as fh:
            for r in dec.rows:
                fh.write(json.dumps(r) + "\n")
        print(f"wrote {len(dec.rows)} rows to {args.jsonl}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
