#!/usr/bin/env python3
"""VU1 micro-program static recompiler (analysis + C++ generator).

Input: micro images dumped by the replay tool (PS2X_VU1_DUMPDIR=<dir>): one
<crc>.bin (16 KB) per distinct micro memory, plus entries.csv (crc,pc,cont).

    python vu1_recomp.py analyze logs/vu1recomp/images
    python vu1_recomp.py generate logs/vu1recomp/images ps2xRuntime/src/lib/vu/generated

The decode mirrors VU1Interpreter::decodeUpperUsage / decodeLowerUsage
(ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp) field for field; the generated code
must match the interpreter bit for bit and cycle for cycle (vu1_bench gate +
PS2X_VU1_RECOMP_VERIFY side-by-side check).
"""

import argparse
import collections
import os
import struct
import sys

FMAC_LAT = 4


def lane(c):
    """Lane bit for component c (x=8, y=4, z=2, w=1), as laneForComponent."""
    return 1 << (3 - c)


def DEST(i): return (i >> 21) & 0xF
def FT(i): return (i >> 16) & 0x1F
def FS(i): return (i >> 11) & 0x1F
def FD(i): return (i >> 6) & 0x1F
def VIT(i): return (i >> 16) & 0xF
def VIS(i): return (i >> 11) & 0xF
def VID(i): return (i >> 6) & 0xF


def IMM11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


class Usage:
    __slots__ = ("vfRead", "vfWrite", "viRead", "viWrite", "accRead", "accWrite", "latency",
                 "vfLatency", "viLatency", "pipeline", "waitQ", "waitP", "readsClip",
                 "writesClip", "delaysNextBranchRead", "reserved", "name")

    def __init__(self):
        self.vfRead = []          # [(reg, lanes)] max 2, merged per reg
        self.vfWrite = (0, 0)
        self.viRead = 0
        self.viWrite = 0
        self.accRead = 0
        self.accWrite = 0
        self.latency = 0
        self.vfLatency = 0
        self.viLatency = 0
        self.pipeline = "none"
        self.waitQ = False
        self.waitP = False
        self.readsClip = False
        self.writesClip = False
        self.delaysNextBranchRead = False
        self.reserved = False
        self.name = "?"

    def addVfRead(self, reg, lanes):
        if lanes == 0:
            return
        for k, (r, l) in enumerate(self.vfRead):
            if r == reg:
                self.vfRead[k] = (r, l | lanes)
                return
        if len(self.vfRead) < 2:
            self.vfRead.append((reg, lanes))

    def addVfWrite(self, reg, lanes):
        if reg == 0 or lanes == 0:
            return
        if self.vfWrite[0] == 0:
            self.vfWrite = (reg, lanes)
        elif self.vfWrite[0] == reg:
            self.vfWrite = (reg, self.vfWrite[1] | lanes)

    def vfReadLanes(self, reg):
        for r, l in self.vfRead:
            if r == reg:
                return l
        return 0


UPPER_BC_NAMES = {0x00: "ADD", 0x04: "SUB", 0x08: "MADD", 0x0C: "MSUB", 0x10: "MAX", 0x14: "MINI", 0x18: "MUL"}
UPPER_NAMES = {0x1C: "MULq", 0x1D: "MAXi", 0x1E: "MULi", 0x1F: "MINIi", 0x20: "ADDq", 0x21: "MADDq", 0x22: "ADDi",
               0x23: "MADDi", 0x24: "SUBq", 0x25: "MSUBq", 0x26: "SUBi", 0x27: "MSUBi", 0x28: "ADD", 0x29: "MADD",
               0x2A: "MUL", 0x2B: "MAX", 0x2C: "SUB", 0x2D: "MSUB", 0x2E: "OPMSUB", 0x2F: "MINI"}
UPPER_SPECIAL_NAMES = {0x10: "ITOF0", 0x11: "ITOF4", 0x12: "ITOF12", 0x13: "ITOF15", 0x14: "FTOI0", 0x15: "FTOI4",
                       0x16: "FTOI12", 0x17: "FTOI15", 0x1C: "MULAq", 0x1D: "ABS", 0x1E: "MULAi", 0x1F: "CLIP",
                       0x20: "ADDAq", 0x21: "MADDAq", 0x22: "ADDAi", 0x23: "MADDAi", 0x24: "SUBAq", 0x25: "MSUBAq",
                       0x26: "SUBAi", 0x27: "MSUBAi", 0x28: "ADDA", 0x29: "MADDA", 0x2A: "MULA", 0x2C: "SUBA",
                       0x2D: "MSUBA", 0x2E: "OPMULA", 0x2F: "NOP", 0x30: "NOP"}
UPPER_SPECIAL_BC = {0x00: "ADDA", 0x04: "SUBA", 0x08: "MADDA", 0x0C: "MSUBA", 0x18: "MULA"}


def upper_name(u):
    op = u & 0x3F
    if op <= 0x1B:
        return UPPER_BC_NAMES[op & 0x1C] + "xyzw"[op & 3]
    if op <= 0x2F:
        return UPPER_NAMES[op]
    if op >= 0x3C:
        sp = (u & 3) | ((u >> 4) & 0x7C)
        if sp <= 0x0F or 0x18 <= sp <= 0x1B:
            return UPPER_SPECIAL_BC[sp & 0x1C] + "xyzw"[sp & 3]
        return UPPER_SPECIAL_NAMES.get(sp, "RES%02x" % sp)
    return "RES"


def decode_upper(u):
    us = Usage()
    us.pipeline = "fmac"
    us.latency = FMAC_LAT
    us.name = upper_name(u)
    op = u & 0x3F
    dest, fs, ft, fd = DEST(u), FS(u), FT(u), FD(u)
    if op <= 0x2F:
        us.addVfRead(fs, dest)
        us.addVfWrite(fd, dest)
        if op <= 0x1B:
            us.addVfRead(ft, lane(op & 3))
        elif op >= 0x28:
            us.addVfRead(ft, 0xE if op == 0x2E else dest)
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E):
            us.accRead = dest
        return us
    if op >= 0x3C:
        sp = (u & 3) | ((u >> 4) & 0x7C)
        writesAcc = sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or 0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E
        if writesAcc:
            us.addVfRead(fs, dest)
            if sp <= 0x1B:
                us.addVfRead(ft, lane(sp & 3))
            elif 0x28 <= sp <= 0x2E:
                us.addVfRead(ft, 0xE if sp == 0x2E else dest)
            us.accWrite = dest
            if 0x08 <= sp <= 0x0F or sp in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D):
                us.accRead = dest
        elif 0x10 <= sp <= 0x17 or sp == 0x1D:
            us.addVfRead(fs, dest)
            us.addVfWrite(ft, dest)
        elif sp == 0x1F:
            us.addVfRead(fs, 0xE)
            us.addVfRead(ft, 0x1)
            us.writesClip = True
        elif sp not in (0x2F, 0x30):
            us.reserved = True
        return us
    us.reserved = True
    return us


LOWER_SPECIAL_NAMES = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD", 0x38: "DIV",
                       0x39: "SQRT", 0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR", 0x3E: "ILWR",
                       0x3F: "ISWR", 0x40: "RNEXT", 0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR", 0x64: "MFP",
                       0x68: "XTOP", 0x69: "XITOP", 0x6C: "XGKICK", 0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG",
                       0x73: "ERLENG", 0x74: "EATANxy", 0x75: "EATANxz", 0x76: "ESUM", 0x77: "ERSQRT",
                       0x78: "ESQRT", 0x79: "ESIN", 0x7A: "ERCPR", 0x7B: "WAITP", 0x7C: "EATAN", 0x7D: "EEXP"}
LOWER_NAMES = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ",
               0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
               0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR",
               0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
EFU_LAT = {0x70: 11, 0x71: 18, 0x72: 18, 0x77: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x7C: 54, 0x76: 12, 0x78: 12,
           0x7A: 12, 0x79: 29, 0x7D: 44}


def decode_lower(l):
    us = Usage()
    if l == 0 or l == 0x8000033C:
        us.name = "NOP"
        return us
    opHi = (l >> 25) & 0x7F
    vfT, vfS, viT, viS, viD, dest = FT(l), FS(l), VIT(l), VIS(l), VID(l), DEST(l)

    def rd(r):
        if r:
            us.viRead |= 1 << r

    def wr(r):
        if r:
            us.viWrite |= 1 << r

    if opHi != 0x40:
        us.name = LOWER_NAMES.get(opHi, "RES")
    if opHi == 0x00:
        us.pipeline, us.latency = "lsu", 4; rd(viS); us.addVfWrite(vfT, dest)
    elif opHi == 0x01:
        us.pipeline, us.latency = "lsu", 1; rd(viT); us.addVfRead(vfS, dest)
    elif opHi == 0x04:
        us.pipeline, us.latency = "lsu", 4; rd(viS); wr(viT)
    elif opHi == 0x05:
        us.pipeline, us.latency = "lsu", 1; rd(viS); rd(viT)
    elif opHi in (0x08, 0x09):
        us.pipeline, us.latency = "ialu", 1; us.delaysNextBranchRead = True; rd(viS); wr(viT)
    elif opHi in (0x10, 0x12, 0x13):
        us.pipeline, us.latency = "ialu", 1; us.readsClip = True; wr(1)
    elif opHi == 0x11:
        us.pipeline, us.latency = "fmac", FMAC_LAT; us.writesClip = True
    elif opHi in (0x14, 0x16, 0x17):
        us.pipeline, us.latency = "ialu", 1; wr(viT)
    elif opHi == 0x15:
        us.pipeline, us.latency = "fmac", FMAC_LAT
    elif opHi in (0x18, 0x1A, 0x1B):
        us.pipeline, us.latency = "ialu", 1; rd(viS); wr(viT)
    elif opHi == 0x1C:
        us.pipeline, us.latency = "ialu", 1; us.readsClip = True; wr(viT)
    elif opHi == 0x20:
        us.pipeline = "branch"
    elif opHi == 0x21:
        us.pipeline, us.latency = "branch", 1; wr(viT)
    elif opHi == 0x24:
        us.pipeline = "branch"; rd(viS)
    elif opHi == 0x25:
        us.pipeline, us.latency = "branch", 1; rd(viS); wr(viT)
    elif opHi in (0x28, 0x29):
        us.pipeline = "branch"; rd(viS); rd(viT)
    elif opHi in (0x2C, 0x2D, 0x2E, 0x2F):
        us.pipeline = "branch"; rd(viS)
    elif opHi == 0x40:
        direct = l & 0x3F
        if direct in (0x30, 0x31, 0x34, 0x35):
            us.name = {0x30: "IADD", 0x31: "ISUB", 0x34: "IAND", 0x35: "IOR"}[direct]
            us.pipeline, us.latency = "ialu", 1; us.delaysNextBranchRead = True; rd(viS); rd(viT); wr(viD)
            return us
        if direct == 0x32:
            us.name = "IADDI"
            us.pipeline, us.latency = "ialu", 1; us.delaysNextBranchRead = True; rd(viS); wr(viT)
            return us
        if direct < 0x3C:
            us.name = "RES"; us.reserved = True
            return us
        sp = (l & 3) | ((l >> 4) & 0x7C)
        us.name = LOWER_SPECIAL_NAMES.get(sp, "RES%02x" % sp)
        if sp in (0x30, 0x31):
            us.pipeline, us.latency = "fmac", 4; us.addVfRead(vfS, 0xF if sp == 0x31 else dest); us.addVfWrite(vfT, dest)
        elif sp in (0x34, 0x36):
            us.pipeline, us.latency, us.viLatency = "lsu", 4, 1; us.delaysNextBranchRead = True
            rd(viS); wr(viS); us.addVfWrite(vfT, dest)
        elif sp in (0x35, 0x37):
            us.pipeline, us.latency = "lsu", 1; us.delaysNextBranchRead = True; rd(viT); wr(viT); us.addVfRead(vfS, dest)
        elif sp == 0x38:
            us.pipeline, us.latency = "fdiv", 7
            us.addVfRead(vfS, lane((l >> 21) & 3)); us.addVfRead(vfT, lane((l >> 23) & 3))
        elif sp == 0x39:
            us.pipeline, us.latency = "fdiv", 7; us.addVfRead(vfT, lane((l >> 23) & 3))
        elif sp == 0x3A:
            us.pipeline, us.latency = "fdiv", 13
            us.addVfRead(vfS, lane((l >> 21) & 3)); us.addVfRead(vfT, lane((l >> 23) & 3))
        elif sp == 0x3B:
            us.pipeline = "fdiv"; us.waitQ = True
        elif sp == 0x3C:
            us.pipeline, us.latency = "ialu", 1; us.delaysNextBranchRead = True
            us.addVfRead(vfS, lane((l >> 21) & 3)); wr(viT)
        elif sp == 0x3D:
            us.pipeline, us.latency = "fmac", 4; rd(viS); us.addVfWrite(vfT, dest)
        elif sp == 0x3E:
            us.pipeline, us.latency = "lsu", 4; rd(viS); wr(viT)
        elif sp == 0x3F:
            us.pipeline, us.latency = "lsu", 1; rd(viS); rd(viT)
        elif sp in (0x40, 0x41):
            us.pipeline, us.latency = "fmac", 4; us.addVfWrite(vfT, dest)
        elif sp in (0x42, 0x43):
            us.pipeline, us.latency = "ialu", 1; us.addVfRead(vfS, lane((l >> 21) & 3))
        elif sp == 0x64:
            us.pipeline, us.latency = "fmac", 4; us.addVfWrite(vfT, dest)
        elif sp in (0x68, 0x69):
            us.pipeline, us.latency = "ialu", 1; wr(viT)
        elif sp == 0x6C:
            us.pipeline, us.latency = "xgkick", 2; rd(viS)
        elif sp in EFU_LAT:
            us.pipeline, us.latency = "efu", EFU_LAT[sp]
            if 0x70 <= sp <= 0x73:
                us.addVfRead(vfS, 0xE)
            elif sp == 0x74:
                us.addVfRead(vfS, 0xC)
            elif sp == 0x75:
                us.addVfRead(vfS, 0xA)
            elif sp == 0x76:
                us.addVfRead(vfS, 0xF)
            else:
                us.addVfRead(vfS, lane((l >> 21) & 3))
        elif sp == 0x7B:
            us.pipeline = "efu"; us.waitP = True
        else:
            us.reserved = True
    else:
        us.reserved = True
    return us


class Pair:
    def __init__(self, img, pc):
        self.pc = pc
        self.lower, self.upper = struct.unpack_from("<II", img, pc)
        u = self.upper
        self.iBit = bool(u & 0x80000000)
        self.eBit = bool(u & 0x40000000)
        self.mBit = bool(u & 0x20000000)
        self.dBit = bool(u & 0x10000000)
        self.tBit = bool(u & 0x08000000)
        self.up = decode_upper(u)
        self.lo = decode_lower(self.lower) if not self.iBit else Usage()
        if self.iBit:
            self.lo.name = "LOI"
        self.shadowReg = 0
        self.suppressedLowerVf = 0
        uw = self.up.vfWrite[0]
        if uw != 0 and (self.lo.vfReadLanes(uw) != 0 or self.lo.vfWrite[0] == uw):
            self.shadowReg = uw
            if self.lo.vfWrite[0] == uw:
                self.suppressedLowerVf = uw

    @property
    def lowerOpHi(self):
        return (self.lower >> 25) & 0x7F

    def is_branch(self):
        return not self.iBit and self.lo.pipeline == "branch"

    def branch_target(self):
        """Static target byte pc (None for JR/JALR)."""
        if self.lowerOpHi in (0x24, 0x25):
            return None
        return (self.pc + 8 + IMM11(self.lower) * 8) & 0x3FFF

    def is_cond_branch(self):
        return self.lowerOpHi in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F)

    def text(self):
        lo = self.lo.name
        if self.iBit:
            lo = "LOI %08x" % self.lower
        flags = "".join(c for c, b in (("I", self.iBit), ("E", self.eBit), ("M", self.mBit), ("D", self.dBit), ("T", self.tBit)) if b)
        return "%04x  %-10s %-10s %s" % (self.pc, self.up.name, lo, flags)


def load_images(d):
    imgs = {}
    for name in sorted(os.listdir(d)):
        if name.endswith(".bin"):
            imgs[name[:-4]] = open(os.path.join(d, name), "rb").read()
    entries = collections.defaultdict(set)
    ep = os.path.join(d, "entries.csv")
    if os.path.exists(ep):
        for line in open(ep):
            crc, pc, cont = line.strip().split(",")
            entries[crc].add(int(pc))
    return imgs, entries


def explore(img, starts):
    """Reachable pairs from the entry pcs. Returns (pairs dict, issues list)."""
    pairs = {}
    issues = []
    work = list(starts)
    seen = set()
    while work:
        pc = work.pop()
        while pc not in seen and pc + 8 <= len(img):
            seen.add(pc)
            p = Pair(img, pc)
            pairs[pc] = p
            if p.up.reserved or p.lo.reserved:
                issues.append("reserved at %04x" % pc)
                break
            if p.is_branch():
                ds = pc + 8
                dp = Pair(img, ds)
                if dp.is_branch():
                    issues.append("branch in delay slot at %04x" % ds)
                if dp.eBit:
                    issues.append("E-bit in delay slot at %04x" % ds)
                t = p.branch_target()
                if t is None:
                    issues.append("JR/JALR at %04x" % pc)
                else:
                    work.append(t)
                if p.lowerOpHi == 0x21:  # BAL: return address = pc+16
                    work.append(pc + 16)
                if p.lowerOpHi in (0x20, 0x21, 0x24, 0x25):
                    seen.add(ds)
                    pairs[ds] = dp
                    break
                pc += 8
                continue
            if p.eBit:
                # E-bit: the next pair (delay slot) runs, then the program ends;
                # MSCNT resumes after it.
                seen.add(pc + 8)
                pairs[pc + 8] = Pair(img, pc + 8)
                break
            pc += 8
    return pairs, issues


def analyze(args):
    imgs, entries = load_images(args.dir)
    total = collections.Counter()
    for crc, img in imgs.items():
        starts = sorted(entries.get(crc, {0}))
        pairs, issues = explore(img, starts)
        ops = collections.Counter()
        for p in pairs.values():
            ops[p.up.name] += 1
            ops[p.lo.name] += 1
            if p.dBit or p.tBit:
                issues.append("D/T bit at %04x" % p.pc)
            if p.mBit:
                ops["<M-bit>"] += 1
        total.update(ops)
        print("image %s: entries %s, %d reachable pairs" % (crc, starts, len(pairs)))
        for i in sorted(set(issues)):
            print("   !", i)
        if args.listing:
            for pc in sorted(pairs):
                print("   " + pairs[pc].text())
    print("\nop histogram (all images):")
    for name, n in sorted(total.items(), key=lambda kv: -kv[1]):
        print("  %-10s %d" % (name, n))


# ---------------------------------------------------------------------------
# C++ generation
# ---------------------------------------------------------------------------

LANES = "xyzw"
CROSS_L = (1, 2, 0, 3)
CROSS_R = (2, 0, 1, 3)


def dest_lanes(dest):
    return [c for c in range(4) if dest & lane(c)]


def fmac_form(u):
    """(kind, right) for FMAC upper ops, as decodeFmacExactForm; None otherwise."""
    op = u & 0x3F
    special = op >= 0x3C
    code = ((u & 3) | ((u >> 4) & 0x7C)) if special else op
    if code <= 0x0F or 0x18 <= code <= 0x1B:
        if special and 0x10 <= code <= 0x17:
            return None
        if code <= 0x03:
            return ("kAdd", "bc")
        if code <= 0x07:
            return ("kSub", "bc")
        if code <= 0x0B:
            return ("kMadd", "bc")
        if code <= 0x0F:
            return ("kMsub", "bc")
        return ("kMul", "bc")
    table = {0x1C: ("kMul", "q"), 0x1E: ("kMul", "i"), 0x20: ("kAdd", "q"), 0x21: ("kMadd", "q"),
             0x22: ("kAdd", "i"), 0x23: ("kMadd", "i"), 0x24: ("kSub", "q"), 0x25: ("kMsub", "q"),
             0x26: ("kSub", "i"), 0x27: ("kMsub", "i"), 0x28: ("kAdd", "lane"), 0x29: ("kMadd", "lane"),
             0x2A: ("kMul", "lane"), 0x2C: ("kSub", "lane"), 0x2D: ("kMsub", "lane")}
    if code == 0x2E:
        return ("kOpmula" if special else "kOpmsub", "lane")
    return table.get(code)


class Gen:
    def __init__(self, crc, img, entries):
        self.crc = crc
        self.img = img
        self.entries = sorted(entries)
        self.pairs, self.issues = explore(img, self.entries)
        self.out = []

    def w(self, s=""):
        self.out.append(s)

    # --- analysis for the generator -------------------------------------

    def check(self):
        bad = [i for i in self.issues if not i.startswith("JR/JALR")]
        for p in self.pairs.values():
            if p.dBit or p.tBit:
                bad.append("D/T bit at %04x" % p.pc)
            if p.eBit and p.is_branch():
                bad.append("E-bit on a branch at %04x" % p.pc)
            if p.eBit and (p.pc + 8) in self.pairs and self.pairs[p.pc + 8].is_branch():
                bad.append("branch in E-bit delay slot at %04x" % (p.pc + 8))
        return bad

    def build_cfg(self):
        pcs = sorted(self.pairs)
        self.delay_of = {}      # delay-slot pc -> branch pair
        self.ebit_slot = set()  # pcs of pairs that run in an E-bit delay slot
        self.labels = set(self.entries)
        self.jr_targets = set()
        for pc in pcs:
            p = self.pairs[pc]
            if p.is_branch():
                self.delay_of[pc + 8] = p
                t = p.branch_target()
                if t is not None:
                    self.labels.add(t)
                if p.lowerOpHi == 0x21:
                    self.jr_targets.add(pc + 16)
                    self.labels.add(pc + 16)
                if p.is_cond_branch():
                    self.labels.add(pc + 16)
            if p.eBit:
                self.ebit_slot.add(pc + 8)
        # A JR/JALR may land on any BAL return point; keep every label reachable.
        self.jr_targets |= set(self.labels)
        # Pairs whose VI read goes through readBranchVi.
        self.bk_reader = {pc for pc, p in self.pairs.items()
                          if p.is_branch() and (p.lo.viRead & 0xFFFE)}

    def successors(self, pc):
        """Pairs that can execute right after pc (None = next program run)."""
        if pc in self.ebit_slot:
            return [None]
        b = self.delay_of.get(pc)
        if b is not None:
            t = b.branch_target()
            succ = []
            if t is None:
                succ += sorted(self.jr_targets)
            else:
                succ.append(t)
            if b.is_cond_branch():
                succ.append(pc + 8)
            return succ
        return [pc + 8]

    def needs_bk_update(self, pc):
        for s in self.successors(pc):
            if s is None or s in self.bk_reader:
                return True
        return False

    # --- emission helpers -----------------------------------------------

    @staticmethod
    def vfx(reg, c):
        return "vf[%d][%d]" % (reg, c)

    def rb(self, reg):
        """readBranchVi(reg) as an expression."""
        if reg == 0:
            return "0"
        return "(bkReg == %d ? bkVal : vi[%d])" % (reg, reg)

    def emit_stall(self, p):
        terms = []
        for us in (p.up, p.lo):
            for reg, lanes in us.vfRead:
                for c in range(4):
                    if lanes & lane(c):
                        terms.append("r.vfR[%d][%d]" % (reg, c))
            for reg in range(1, 16):
                if us.viRead & (1 << reg):
                    terms.append("r.viR[%d]" % reg)
            for c in range(4):
                if us.accRead & lane(c):
                    terms.append("r.accR[%d]" % c)
        extra = []
        lo = p.lo
        if lo.pipeline == "fdiv":
            extra.append("if (r.fdivValid) rd = std::max(rd, r.fdivReady);")
        if lo.pipeline == "efu":
            extra.append("rd = std::max(rd, r.efuResourceReady);")
        if lo.waitP:
            extra.append("rd = std::max(rd, r.efuWaitReady());")
        if lo.pipeline == "xgkick":
            extra.append("if (r.kickActive) rd = std::max(rd, r.kickFinishAll());")
        if not terms and not extra:
            return
        # de-duplicate, keep order
        seen = set()
        terms = [t for t in terms if not (t in seen or seen.add(t))]
        self.w("        {")
        self.w("            uint32_t rd = r.cyc;")
        for t in terms:
            self.w("            rd = std::max(rd, %s);" % t)
        for e in extra:
            self.w("            " + e)
        self.w("            r.cyc = rd;")
        self.w("        }")

    def emit_upper(self, p, deferred):
        """Upper op. deferred: VF result goes to u[] (written after the lower)."""
        u = p.upper
        op = u & 0x3F
        if (u & 0x7FF) in (0x2FF, 0x33C):
            return
        special = op >= 0x3C
        code = ((u & 3) | ((u >> 4) & 0x7C)) if special else op
        dest, fs, ft, fd = DEST(u), FS(u), FT(u), FD(u)
        lanes = dest_lanes(dest)
        form = fmac_form(u)
        writes_acc = p.up.accWrite != 0
        if writes_acc:
            tgt = "acc"
        elif special:
            tgt = "vf[%d]" % ft
        else:
            tgt = "vf[%d]" % fd
        out = "u" if deferred else tgt

        def right(c):
            if form[1] == "bc":
                return "bc_"
            if form[1] == "q":
                return "q_"
            if form[1] == "i":
                return "i_"
            return "N(vf[%d][%d])" % (ft, CROSS_R[c] if form[0] in ("kOpmsub", "kOpmula") else c)

        def left(c):
            return "N(vf[%d][%d])" % (fs, CROSS_L[c] if form[0] in ("kOpmsub", "kOpmula") else c)

        self.w("        { // %s" % p.up.name)
        if form is not None:
            if not lanes:
                self.w("        }")
                return
            kind, rk = form
            if rk == "bc":
                self.w("            const float bc_ = N(vf[%d][%d]);" % (ft, code & 3))
            elif rk == "q":
                self.w("            const float q_ = N(r.q());")
            elif rk == "i":
                self.w("            const float i_ = N(s.i);")
            self.w("            uint8_t lf[4] = {0, 0, 0, 0};")
            for c in lanes:
                a = "N(acc[%d])" % c if kind in ("kMadd", "kMsub", "kOpmsub") else "0.0f"
                opmw = "true" if (c == 3 and kind in ("kOpmsub", "kOpmula")) else "false"
                self.w("            const float r%d = lane<%s>(%s, %s, %s, %s, lf[%d]);" % (c, kind, left(c), right(c), a, opmw, c))
            ex = "0u"
            if kind in ("kMadd", "kMsub", "kOpmsub"):
                ex = " | ".join("productSticky(%s, %s)" % (left(c), right(c)) for c in lanes)
            self.w("            r.fmacFlags(lf, %d, %s);" % (dest, ex))
            for c in lanes:
                self.w("            %s[%d] = r%d;" % (out, c, c))
            self.w("        }")
            return

        # Non-FMAC-flag upper ops.
        if not special and (0x10 <= code <= 0x17 or code in (0x1D, 0x1F, 0x2B, 0x2F)):
            is_max = code in (0x10, 0x11, 0x12, 0x13, 0x1D, 0x2B)
            if code <= 0x17:
                self.w("            const float b_ = N(vf[%d][%d]);" % (ft, code & 3))
                bexpr = lambda c: "b_"
            elif code in (0x1D, 0x1F):
                self.w("            const float b_ = N(s.i);")
                bexpr = lambda c: "b_"
            else:
                bexpr = lambda c: "N(vf[%d][%d])" % (ft, c)
            for c in lanes:
                self.w("            const float s%d = N(vf[%d][%d]), t%d = %s;" % (c, fs, c, c, bexpr(c)))
            for c in lanes:
                cmp = ">" if is_max else "<"
                self.w("            const float r%d = (s%d %s t%d) ? s%d : t%d;" % (c, c, cmp, c, c, c))
            for c in lanes:
                self.w("            %s[%d] = r%d;" % (out, c, c))
            self.w("        }")
            return
        if special and 0x10 <= code <= 0x13:  # ITOF
            scale = {0x10: None, 0x11: "16.0f", 0x12: "4096.0f", 0x13: "32768.0f"}[code]
            for c in lanes:
                e = "static_cast<float>(static_cast<int32_t>(fBits(vf[%d][%d])))" % (fs, c)
                if scale:
                    e += " / " + scale
                self.w("            const float r%d = %s;" % (c, e))
            for c in lanes:
                self.w("            %s[%d] = r%d;" % (out, c, c))
            self.w("        }")
            return
        if special and 0x14 <= code <= 0x17:  # FTOI
            scale = {0x14: "1.0f", 0x15: "16.0f", 0x16: "4096.0f", 0x17: "32768.0f"}[code]
            for c in lanes:
                self.w("            const uint32_t r%d = static_cast<uint32_t>(floatToInt(N(vf[%d][%d]), %s));" % (c, fs, c, scale))
            for c in lanes:
                self.w("            std::memcpy(&%s[%d], &r%d, 4);" % (out, c, c))
            self.w("        }")
            return
        if special and code == 0x1D:  # ABS
            for c in lanes:
                self.w("            const float r%d = std::fabs(N(vf[%d][%d]));" % (c, fs, c))
            for c in lanes:
                self.w("            %s[%d] = r%d;" % (out, c, c))
            self.w("        }")
            return
        if special and code == 0x1F:  # CLIP
            self.w("            r.clip(clipFlags(vf[%d], vf[%d]));" % (fs, ft))
            self.w("        }")
            return
        raise SystemExit("unhandled upper %08x (%s) at %04x" % (u, p.up.name, p.pc))

    def emit_lower(self, p, vt_target):
        """Lower op. vt_target(reg): VF write target (scratch when suppressed)."""
        l = p.lower
        if l == 0 or l == 0x8000033C:
            return
        opHi = (l >> 25) & 0x7F
        vfT, vfS, viT, viS, viD, dest = FT(l), FS(l), VIT(l), VIS(l), VID(l), DEST(l)
        imm11 = IMM11(l)
        pc = p.pc
        W = self.w
        W("        { // %s" % p.lo.name)

        def setvi(reg, expr):
            if reg != 0:
                W("            vi[%d] = %s;" % (reg, expr))

        if opHi == 0x00:  # LQ
            W("            r.load(%s, r.addr(vi[%d] + %d), %d);" % (vt_target(vfT), viS, imm11, dest))
        elif opHi == 0x01:  # SQ
            W("            r.storeVf(r.addr(vi[%d] + %d), vf[%d], %d);" % (viT, imm11, vfS, dest))
        elif opHi == 0x04:  # ILW
            if viT:
                W("            vi[%d] = r.ilw(r.addr(vi[%d] + %d), %d);" % (viT, viS, imm11, dest))
        elif opHi == 0x05:  # ISW
            W("            r.storeVi(r.addr(vi[%d] + %d), vi[%d], %d);" % (viS, imm11, viT, dest))
        elif opHi in (0x08, 0x09):  # IADDIU / ISUBIU
            imm = (l & 0x7FF) | ((l >> 10) & 0x7800)
            setvi(viT, "static_cast<int16_t>(vi[%d] %s %d)" % (viS, "+" if opHi == 0x08 else "-", imm))
        elif opHi == 0x10:
            setvi(1, "((r.clipFlag() & 0xFFFFFFu) == 0x%xu) ? 1 : 0" % (l & 0xFFFFFF))
        elif opHi == 0x11:
            W("            r.fcset(0x%xu);" % (l & 0xFFFFFF))
        elif opHi == 0x12:
            setvi(1, "((r.clipFlag() & 0x%xu) != 0u) ? 1 : 0" % (l & 0xFFFFFF))
        elif opHi == 0x13:
            setvi(1, "((r.clipFlag() | 0x%xu) == 0xFFFFFFu) ? 1 : 0" % (l & 0xFFFFFF))
        elif opHi in (0x14, 0x15, 0x16, 0x17):
            imm12 = (((l >> 21) & 1) << 11) | (l & 0x7FF)
            if opHi == 0x15:
                W("            r.fsset(0x%xu);" % imm12)
            elif opHi == 0x14:
                setvi(viT, "((r.status() & 0xFFFu) == 0x%xu) ? 1 : 0" % imm12)
            elif opHi == 0x16:
                setvi(viT, "static_cast<int32_t>((r.status() & 0xFFFu) & 0x%xu)" % imm12)
            else:
                setvi(viT, "static_cast<int32_t>((r.status() & 0xFFFu) | 0x%xu)" % imm12)
        elif opHi == 0x18:
            setvi(viT, "((r.mac() & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(vi[%d]))) ? 1 : 0" % viS)
        elif opHi == 0x1A:
            setvi(viT, "static_cast<int32_t>(r.mac() & static_cast<uint32_t>(static_cast<uint16_t>(vi[%d])))" % viS)
        elif opHi == 0x1B:
            setvi(viT, "static_cast<int32_t>(r.mac() | static_cast<uint32_t>(static_cast<uint16_t>(vi[%d])))" % viS)
        elif opHi == 0x1C:
            setvi(viT, "static_cast<int32_t>(r.clipFlag() & 0x0FFFu)")
        elif opHi == 0x20:
            W("            tk = true;")
        elif opHi == 0x21:
            setvi(viT, "%d" % ((pc + 16) // 8))
            W("            tk = true;")
        elif opHi in (0x24, 0x25):
            W("            jt = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 8u) & 0x3FFFu;" % self.rb(viS))
            if opHi == 0x25:
                setvi(viT, "%d" % ((pc + 16) // 8))
            W("            tk = true;")
        elif opHi in (0x28, 0x29):
            op = "==" if opHi == 0x28 else "!="
            W("            tk = static_cast<int16_t>(%s) %s static_cast<int16_t>(%s);" % (self.rb(viS), op, self.rb(viT)))
        elif opHi in (0x2C, 0x2D, 0x2E, 0x2F):
            op = {0x2C: "<", 0x2D: ">", 0x2E: "<=", 0x2F: ">="}[opHi]
            W("            tk = static_cast<int16_t>(%s) %s 0;" % (self.rb(viS), op))
        elif opHi == 0x40:
            direct = l & 0x3F
            if direct in (0x30, 0x31):
                setvi(viD, "static_cast<int16_t>(vi[%d] %s vi[%d])" % (viS, "+" if direct == 0x30 else "-", viT))
            elif direct == 0x32:
                imm5 = (l >> 6) & 0x1F
                if imm5 & 0x10:
                    imm5 -= 32
                setvi(viT, "static_cast<int16_t>(vi[%d] + %d)" % (viS, imm5))
            elif direct == 0x34:
                setvi(viD, "vi[%d] & vi[%d]" % (viS, viT))
            elif direct == 0x35:
                setvi(viD, "vi[%d] | vi[%d]" % (viS, viT))
            else:
                sp = (l & 3) | ((l >> 4) & 0x7C)
                self.emit_lower_special(p, sp, vt_target)
        else:
            raise SystemExit("unhandled lower %08x at %04x" % (l, pc))
        W("        }")

    def emit_lower_special(self, p, sp, vt_target):
        l = p.lower
        W = self.w
        vfT, vfS, viT, viS, dest = FT(l), FS(l), VIT(l), VIS(l), DEST(l)
        fsf, ftf = (l >> 21) & 3, (l >> 23) & 3
        T = vt_target(vfT)

        def apply(vals):
            for c in dest_lanes(dest):
                W("            %s[%d] = %s;" % (T, c, vals[c]))

        if sp == 0x30:  # MOVE
            W("            const float m0 = vf[%d][0], m1 = vf[%d][1], m2 = vf[%d][2], m3 = vf[%d][3];" % ((vfS,) * 4))
            apply(["m0", "m1", "m2", "m3"])
        elif sp == 0x31:  # MR32
            W("            const float m0 = vf[%d][1], m1 = vf[%d][2], m2 = vf[%d][3], m3 = vf[%d][0];" % ((vfS,) * 4))
            apply(["m0", "m1", "m2", "m3"])
        elif sp == 0x34:  # LQI
            W("            r.load(%s, r.addr(static_cast<uint16_t>(vi[%d])), %d);" % (T, viS, dest))
            if viS:
                W("            vi[%d] = static_cast<int16_t>(vi[%d] + 1);" % (viS, viS))
        elif sp == 0x35:  # SQI
            W("            r.storeVf(r.addr(static_cast<uint16_t>(vi[%d])), vf[%d], %d);" % (viT, vfS, dest))
            if viT:
                W("            vi[%d] = static_cast<int16_t>(vi[%d] + 1);" % (viT, viT))
        elif sp == 0x36:  # LQD
            if viS:
                W("            vi[%d] = static_cast<int16_t>(vi[%d] - 1);" % (viS, viS))
            W("            r.load(%s, r.addr(static_cast<uint16_t>(vi[%d])), %d);" % (T, viS, dest))
        elif sp == 0x37:  # SQD
            if viT:
                W("            vi[%d] = static_cast<int16_t>(vi[%d] - 1);" % (viT, viT))
            W("            r.storeVf(r.addr(static_cast<uint16_t>(vi[%d])), vf[%d], %d);" % (viT, vfS, dest))
        elif sp == 0x38:  # DIV
            W("            const float num = N(vf[%d][%d]), den = N(vf[%d][%d]);" % (vfS, fsf, vfT, ftf))
            W("            uint32_t di = 0u; float res;")
            W("            if (den == 0.0f) { di = num == 0.0f ? 0x10u : 0x20u;")
            W("                res = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max(); }")
            W("            else res = num / den;")
            W("            r.queueQ(res, 7u, di);")
        elif sp == 0x39:  # SQRT
            W("            const float val = N(vf[%d][%d]);" % (vfT, ftf))
            W("            r.queueQ(std::sqrt(std::fabs(val)), 7u, val < 0.0f ? 0x10u : 0u);")
        elif sp == 0x3A:  # RSQRT
            W("            const float num = N(vf[%d][%d]), rad = N(vf[%d][%d]);" % (vfS, fsf, vfT, ftf))
            W("            const float den = std::sqrt(std::fabs(rad));")
            W("            uint32_t di = rad < 0.0f ? 0x10u : 0u; float res = 0.0f;")
            W("            if (den != 0.0f) res = num / den;")
            W("            else { di = num == 0.0f ? 0x10u : 0x20u;")
            W("                res = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max(); }")
            W("            r.queueQ(res, 13u, di);")
        elif sp in (0x3B, 0x7B):  # WAITQ / WAITP
            pass
        elif sp == 0x3C:  # MTIR
            if viT:
                W("            vi[%d] = static_cast<int32_t>(static_cast<int16_t>(fBits(vf[%d][%d]) & 0xFFFFu));" % (viT, vfS, fsf))
        elif sp == 0x3D:  # MFIR
            W("            const float m0 = bitsF(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(vi[%d] & 0xFFFF))));" % viS)
            apply(["m0"] * 4)
        elif sp == 0x3E:  # ILWR
            if viT:
                W("            vi[%d] = r.ilw(r.addr(static_cast<uint16_t>(vi[%d])), %d);" % (viT, viS, dest))
        elif sp == 0x3F:  # ISWR
            W("            r.storeVi(r.addr(static_cast<uint16_t>(vi[%d])), vi[%d], %d);" % (viS, viT, dest))
        elif sp == 0x40:  # RNEXT
            W("            const uint32_t x = (s.r >> 4) & 1u, y = (s.r >> 22) & 1u;")
            W("            s.r = (((s.r << 1) ^ x ^ y) & 0x007FFFFFu) | 0x3F800000u;")
            W("            const float m0 = bitsF(s.r);")
            apply(["m0"] * 4)
        elif sp == 0x41:  # RGET
            W("            const float m0 = bitsF(s.r);")
            apply(["m0"] * 4)
        elif sp == 0x42:  # RINIT
            W("            s.r = 0x3F800000u | (fBits(vf[%d][%d]) & 0x007FFFFFu);" % (vfS, fsf))
        elif sp == 0x43:  # RXOR
            W("            s.r = 0x3F800000u | ((s.r ^ fBits(vf[%d][%d])) & 0x007FFFFFu);" % (vfS, fsf))
        elif sp == 0x64:  # MFP
            W("            const float m0 = r.p();")
            apply(["m0"] * 4)
        elif sp in (0x68, 0x69):  # XTOP / XITOP
            if viT:
                W("            vi[%d] = static_cast<int32_t>(s.%s & 0x3FFu);" % (viT, "top" if sp == 0x68 else "itop"))
        elif sp == 0x6C:  # XGKICK
            W("            r.kickStart(static_cast<uint32_t>(static_cast<uint16_t>(vi[%d])));" % viS)
        elif sp in (0x70, 0x71, 0x72, 0x73):
            W("            const float x = N(vf[%d][0]), y = N(vf[%d][1]), z = N(vf[%d][2]);" % ((vfS,) * 3))
            W("            const float sum = x * x + y * y + z * z;")
            if sp == 0x70:
                W("            r.queueP(sum, 11u);")
            elif sp == 0x71:
                W("            r.queueP(sum != 0.0f ? 1.0f / sum : sum, 18u);")
            elif sp == 0x72:
                W("            r.queueP(std::sqrt(sum), 18u);")
            else:
                W("            const float len = std::sqrt(sum);")
                W("            r.queueP(len != 0.0f ? 1.0f / len : len, 24u);")
        elif sp in (0x74, 0x75):
            other = 1 if sp == 0x74 else 2
            W("            const float x = N(vf[%d][0]), o = N(vf[%d][%d]);" % (vfS, vfS, other))
            W("            r.queueP(x != 0.0f ? eatan(o / x) : 0.0f, 54u);")
        elif sp == 0x76:  # ESUM
            W("            float sum = 0.0f;")
            for c in range(4):
                W("            sum += N(vf[%d][%d]);" % (vfS, c))
            W("            r.queueP(sum, 12u);")
        elif sp == 0x77:  # ERSQRT
            W("            float v = N(vf[%d][%d]);" % (vfS, fsf))
            W("            if (v >= 0.0f) { v = std::sqrt(v); if (v != 0.0f) v = 1.0f / v; }")
            W("            r.queueP(v, 18u);")
        elif sp == 0x78:  # ESQRT
            W("            const float v = N(vf[%d][%d]);" % (vfS, fsf))
            W("            r.queueP(v >= 0.0f ? std::sqrt(v) : v, 12u);")
        elif sp == 0x79:
            W("            r.queueP(esin(N(vf[%d][%d])), 29u);" % (vfS, fsf))
        elif sp == 0x7A:
            W("            const float v = N(vf[%d][%d]);" % (vfS, fsf))
            W("            r.queueP(v != 0.0f ? 1.0f / v : v, 12u);")
        elif sp == 0x7C:
            W("            r.queueP(eatan(N(vf[%d][%d])), 54u);" % (vfS, fsf))
        elif sp == 0x7D:
            W("            r.queueP(eexp(N(vf[%d][%d])), 44u);" % (vfS, fsf))
        else:
            raise SystemExit("unhandled lower special %02x at %04x" % (sp, p.pc))

    def pair_writes_vf0(self, p):
        u = p.upper
        if (u & 0x7FF) not in (0x2FF, 0x33C) and not p.up.accWrite and not p.up.writesClip:
            op = u & 0x3F
            special = op >= 0x3C
            reg = FT(u) if special else FD(u)
            if reg == 0 and DEST(u) != 0:
                return True
        if not p.iBit:
            l = p.lower
            opHi = (l >> 25) & 0x7F
            if opHi == 0x00 and FT(l) == 0:
                return True
            if opHi == 0x40 and (l & 0x3F) >= 0x3C:
                sp = (l & 3) | ((l >> 4) & 0x7C)
                if sp in (0x30, 0x31, 0x34, 0x36, 0x3D, 0x40, 0x41, 0x64) and FT(l) == 0:
                    return True
        return False

    def emit_pair(self, p):
        W = self.w
        pc = p.pc
        if pc in self.labels:
            W("    L_%04x:" % pc)
            W("        if (r.cyc >= budget || r.error) { s.pc = 0x%xu; goto stop; }" % pc)
        W("    { // %s" % p.text().strip())
        self.emit_stall(p)
        W("        ++retired;")
        bk = self.needs_bk_update(pc)
        written_vi = 0
        viw = p.lo.viWrite & 0xFFFE
        if viw:
            written_vi = (viw & -viw).bit_length() - 1
        record = bk and written_vi and p.lo.delaysNextBranchRead
        if record:
            W("        const int32_t oldVi = vi[%d];" % written_vi)
        is_branch = p.is_branch()

        deferred = p.shadowReg != 0 and not p.iBit
        if deferred:
            W("        float u[4];")
        self.emit_upper(p, deferred)
        if p.iBit:
            W("        s.i = N(bitsF(0x%08xu));" % p.lower)
        else:
            sup = p.suppressedLowerVf

            def vt_target(reg):
                if sup and reg == sup:
                    return "scratch"
                return "vf[%d]" % reg
            if sup:
                W("        float scratch[4];")
            self.emit_lower(p, vt_target)
        if deferred:
            for c in dest_lanes(DEST(p.upper)):
                W("        vf[%d][%d] = u[%d];" % (p.shadowReg, c, c))
        if self.pair_writes_vf0(p):
            W("        vf[0][0] = 0.0f; vf[0][1] = 0.0f; vf[0][2] = 0.0f; vf[0][3] = 1.0f;")

        # markPairWrites
        lw_reg, lw_lanes = p.lo.vfWrite
        if lw_reg and p.suppressedLowerVf != lw_reg:
            lat = p.lo.vfLatency or p.lo.latency
            for c in range(4):
                if lw_lanes & lane(c):
                    W("        r.vfR[%d][%d] = r.cyc + %du;" % (lw_reg, c, lat))
        uw_reg, uw_lanes = p.up.vfWrite
        if uw_reg:
            lat = p.up.vfLatency or p.up.latency
            for c in range(4):
                if uw_lanes & lane(c):
                    W("        r.vfR[%d][%d] = r.cyc + %du;" % (uw_reg, c, lat))
        if viw:
            lat = p.lo.viLatency or p.lo.latency
            for reg in range(1, 16):
                if viw & (1 << reg):
                    W("        r.viR[%d] = r.cyc + %du;" % (reg, lat))
        for c in range(4):
            if p.up.accWrite & lane(c):
                W("        r.accR[%d] = r.cyc + 1u;" % c)

        if bk:
            if record:
                W("        bkReg = %d; bkVal = oldVi;" % written_vi)
            else:
                W("        bkReg = 0;")
        W("        ++r.cyc;")
        self.emit_flow(p)
        W("    }")

    def emit_flow(self, p):
        W = self.w
        pc = p.pc
        is_branch = p.is_branch()
        # Control flow after this pair.
        b = self.delay_of.get(pc)
        if pc in self.ebit_slot:
            nxt = (pc + 8) if pc + 8 < 0x4000 else 0
            W("        s.pc = 0x%xu;" % nxt)
            W("        goto done;")
        elif b is not None:
            t = b.branch_target()
            if t is None:
                W("        if (tk) { tk = false; goto dispatch_jr; }")
            elif b.is_cond_branch():
                W("        if (tk) { tk = false; goto L_%04x; }" % t)
            else:
                W("        tk = false;")
                W("        goto L_%04x;" % t)
        if not is_branch and pc not in self.ebit_slot and b is None:
            nxt = pc + 8
            if nxt not in self.pairs:
                W("        s.pc = 0x%xu; goto stop; // fell off analysed code" % nxt)
        elif b is not None and b.is_cond_branch():
            if pc + 8 not in self.pairs:
                W("        s.pc = 0x%xu; goto stop; // fell off analysed code" % (pc + 8))

    def generate(self):
        self.build_cfg()
        W = self.w
        W("// Generated by build_scripts/vu1_recomp.py -- do not edit.")
        W("// VU1 program %s, entries %s." % (self.crc, ", ".join("0x%x" % e for e in self.entries)))
        W("// Seen in micro images: %s." % ", ".join(getattr(self, "images", [self.crc])))
        W('#include "Kernel/Vu1Recomp/vu1_recomp_rt.h"')
        W("")
        W("namespace vu1rc")
        W("{")
        W("bool prog_%s(Ctx &ctx)" % self.crc)
        W("{")
        W("    VU1State &s = *ctx.st;")
        W("    switch (s.pc)")
        W("    {")
        for e in self.entries:
            W("    case 0x%xu:" % e)
        W("        break;")
        W("    default:")
        W("        return false;")
        W("    }")
        W("    R r;")
        W("    r.init(ctx);")
        W("    auto &vf = s.vf;")
        W("    auto &vi = s.vi;")
        W("    auto &acc = s.acc;")
        W("    const uint32_t budget = ctx.budget;")
        W("    uint64_t retired = 0;")
        W("    int32_t bkReg = ctx.bkValid ? ctx.bkReg : 0;")
        W("    int32_t bkVal = ctx.bkVal;")
        W("    bool tk = false;")
        W("    uint32_t jt = 0;")
        W("    (void)jt; (void)acc;")
        W("    switch (s.pc)")
        W("    {")
        for e in self.entries:
            W("    case 0x%xu: goto L_%04x;" % (e, e))
        W("    }")
        for pc in sorted(self.pairs):
            self.emit_pair(self.pairs[pc])
        W("dispatch_jr:")
        W("    switch (jt)")
        W("    {")
        for t in sorted(self.jr_targets):
            if t in self.pairs:
                W("    case 0x%xu: goto L_%04x;" % (t, t))
        W("    default:")
        W('        reportError("JR to an address outside the compiled program", s.pc, jt);')
        W("        s.pc = jt;")
        W("        r.error = true;")
        W("        goto stop;")
        W("    }")
        W("done:")
        W("    ctx.cycles = r.finish(r.viMax());")
        W("    ctx.end = kEndEbit;")
        W("    goto out;")
        W("stop:")
        W("    r.finish(r.viMax());")
        W("    ctx.cycles = r.cyc;")
        W("    ctx.end = r.error ? kEndError : kEndCycleLimit;")
        W("out:")
        W("    ctx.retired = retired;")
        W("    ctx.workingClip = r.workingClip;")
        W("    ctx.bkValid = bkReg != 0;")
        W("    ctx.bkReg = static_cast<uint8_t>(bkReg);")
        W("    ctx.bkVal = bkVal;")
        W("    return true;")
        W("}")
        W("}")
        return "\n".join(self.out) + "\n"


CRC_TABLE_SRC = r"""
namespace
{
    struct CrcTable
    {
        uint32_t t[256];
        CrcTable()
        {
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
        }
    };

    uint32_t crc32(const uint8_t *p, uint32_t n)
    {
        static const CrcTable table;
        uint32_t c = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; ++i)
            c = table.t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
}
"""


def code_ranges(img, pairs):
    """Contiguous [start, end) byte ranges of the compiled pairs."""
    ranges = []
    for pc in sorted(pairs):
        if ranges and ranges[-1][1] == pc:
            ranges[-1][1] = pc + 8
        else:
            ranges.append([pc, pc + 8])
    return ranges


def generate(args):
    import hashlib
    import zlib
    imgs, entries = load_images(args.dir)
    os.makedirs(args.out, exist_ok=True)

    # One program per distinct compiled code, whatever else the 16 KB image
    # holds: the game uploads the same programs next to different leftovers.
    # Entries of every image with the same code are merged.
    progs = collections.OrderedDict()  # key -> {img, entries, crcs}
    for crc, img in imgs.items():
        if "%08x" % zlib.crc32(img) != crc:
            raise SystemExit("%s.bin: CRC mismatch (file name is not its crc32)" % crc)
        starts = set(entries.get(crc, {0}))
        pairs, _ = explore(img, sorted(starts))
        h = hashlib.sha1()
        for pc in sorted(pairs):
            h.update(pc.to_bytes(4, "little"))
            h.update(img[pc:pc + 8])
        key = h.hexdigest()[:12]
        pr = progs.setdefault(key, {"img": img, "entries": set(), "crcs": []})
        pr["entries"] |= starts
        pr["crcs"].append(crc)

    done = []  # (key, ranges, img)
    for key, pr in progs.items():
        g = Gen(key, pr["img"], pr["entries"])
        g.images = pr["crcs"]
        bad = g.check()
        if bad:
            print("skip %s (%s): %s" % (key, ",".join(pr["crcs"]), "; ".join(sorted(set(bad)))))
            continue
        src = g.generate()
        path = os.path.join(args.out, "vu1rc_%s.cpp" % key)
        write_if_changed(path, src)
        done.append((key, code_ranges(pr["img"], g.pairs), pr["img"]))
        print("generated %s (%d pairs, %d labels, images %s)" % (path, len(g.pairs), len(g.labels), ",".join(pr["crcs"])))

    # Included by vu1_recomp_rt.cpp when present (.inc, so the Kernel/*.cpp
    # glob does not compile it on its own and a clean clone still links).
    t = ["// Generated by build_scripts/vu1_recomp.py -- do not edit.",
         "#define VU1RC_HAVE_TABLE 1", "", "namespace vu1rc", "{"]
    for key, _, _ in done:
        t.append("bool prog_%s(Ctx &ctx);" % key)
    t.append("}")
    t.append(CRC_TABLE_SRC)
    t.append("namespace")
    t.append("{")
    t.append("    // The micro memory bytes each program was compiled from, as ranges.")
    t.append("    struct CodeRange")
    t.append("    {")
    t.append("        uint32_t start, size;")
    t.append("        const uint8_t *bytes;")
    t.append("    };")
    for key, ranges, img in done:
        for k, (a, b) in enumerate(ranges):
            data = img[a:b]
            t.append("    const uint8_t code_%s_%d[%d] = {" % (key, k, len(data)))
            for off in range(0, len(data), 24):
                t.append("        " + ", ".join("0x%02x" % x for x in data[off:off + 24]) + ",")
            t.append("    };")
        t.append("    const CodeRange ranges_%s[] = {" % key)
        for k, (a, b) in enumerate(ranges):
            t.append("        {0x%xu, %du, code_%s_%d}," % (a, b - a, key, k))
        t.append("    };")
    t.append("    struct Entry")
    t.append("    {")
    t.append("        vu1rc::Program program;")
    t.append("        const CodeRange *ranges;")
    t.append("        uint32_t count;")
    t.append("    };")
    t.append("    const Entry kPrograms[] = {")
    for key, ranges, _ in done:
        t.append("        {vu1rc::prog_%s, ranges_%s, %d}," % (key, key, len(ranges)))
    t.append("    };")
    t.append("}")
    t.append("")
    t.append("uint32_t vu1rc::imageCrc(const uint8_t *microMem, uint32_t size)")
    t.append("{")
    t.append("    return crc32(microMem, size);")
    t.append("}")
    t.append("")
    t.append("uint32_t vu1rc::find(const uint8_t *microMem, uint32_t size, Program *out, uint32_t maxOut)")
    t.append("{")
    t.append("    uint32_t n = 0;")
    t.append("    for (const Entry &e : kPrograms)")
    t.append("    {")
    t.append("        bool match = true;")
    t.append("        for (uint32_t k = 0; k < e.count && match; ++k)")
    t.append("            match = e.ranges[k].start + e.ranges[k].size <= size &&")
    t.append("                    std::memcmp(microMem + e.ranges[k].start, e.ranges[k].bytes, e.ranges[k].size) == 0;")
    t.append("        if (match && n < maxOut)")
    t.append("            out[n++] = e.program;")
    t.append("    }")
    t.append("    return n;")
    t.append("}")
    write_if_changed(os.path.join(args.out, "vu1rc_table.inc"), "\n".join(t) + "\n")
    done = [k for k, _, _ in done]
    # Remove programs for images no longer in the set.
    for name in os.listdir(args.out):
        if name.startswith("vu1rc_") and name.endswith(".cpp"):
            if name[6:-4] not in done:
                os.remove(os.path.join(args.out, name))
                print("removed stale", name)


def write_if_changed(path, text):
    data = text.encode("utf-8")
    if os.path.exists(path) and open(path, "rb").read() == data:
        return
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("analyze")
    a.add_argument("dir")
    a.add_argument("--listing", action="store_true")
    a.set_defaults(fn=analyze)
    g = sub.add_parser("generate")
    g.add_argument("dir")
    g.add_argument("out")
    g.set_defaults(fn=generate)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
