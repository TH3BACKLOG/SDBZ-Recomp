"""R5900 decode/classify/encode for the opcode conformance test (Stage B).

classify(word) -> Op(cls, groups, text) or a skip reason (str).
  cls    = class key (mnemonic, e.g. "add.s", "vmaddx", "paddw"); one class = one translator path.
  groups = register files the word touches: "G" (GPR + HI/LO + SA), "F" (FPR + FCR31 + FPU ACC),
           "V" (VF + VI/control + VU ACC). The test loads and stores exactly these files.

Only computational words are tested: no branches/jumps, loads/stores, COP0, cache/sync,
syscalls/traps, VU0 microprogram calls or VU0 data-memory ops.
"""
from dataclasses import dataclass

GPRN = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
K0, K1 = 26, 27  # input / output pointers of every test

# kind = operand shape, used for the k0/k1 check and the listing
SPECIAL = {
    0x00: ("sll", "SH"), 0x02: ("srl", "SH"), 0x03: ("sra", "SH"),
    0x04: ("sllv", "SHV"), 0x06: ("srlv", "SHV"), 0x07: ("srav", "SHV"),
    0x0A: ("movz", "R3"), 0x0B: ("movn", "R3"),
    0x10: ("mfhi", "RD"), 0x11: ("mthi", "RS"), 0x12: ("mflo", "RD"), 0x13: ("mtlo", "RS"),
    0x14: ("dsllv", "SHV"), 0x16: ("dsrlv", "SHV"), 0x17: ("dsrav", "SHV"),
    0x18: ("mult", "R3"), 0x19: ("multu", "R3"), 0x1A: ("div", "RSRT"), 0x1B: ("divu", "RSRT"),
    0x21: ("addu", "R3"), 0x23: ("subu", "R3"), 0x24: ("and", "R3"), 0x25: ("or", "R3"),
    0x26: ("xor", "R3"), 0x27: ("nor", "R3"), 0x28: ("mfsa", "RD"), 0x29: ("mtsa", "RS"),
    0x2A: ("slt", "R3"), 0x2B: ("sltu", "R3"), 0x2D: ("daddu", "R3"), 0x2F: ("dsubu", "R3"),
    0x38: ("dsll", "SH"), 0x3A: ("dsrl", "SH"), 0x3B: ("dsra", "SH"),
    0x3C: ("dsll32", "SH"), 0x3E: ("dsrl32", "SH"), 0x3F: ("dsra32", "SH"),
}
SPECIAL_SKIP = {0x08: "jump", 0x09: "jump", 0x0C: "syscall", 0x0D: "break", 0x0F: "sync",
                0x20: "add traps on overflow", 0x22: "sub traps on overflow",
                0x2C: "dadd traps on overflow", 0x2E: "dsub traps on overflow"}
IMM = {0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori",
       0x0F: "lui", 0x19: "daddiu"}
REGIMM = {0x18: "mtsab", 0x19: "mtsah"}

MMI = {
    0x00: ("madd", "R3"), 0x01: ("maddu", "R3"), 0x04: ("plzcw", "RDRS"),
    0x10: ("mfhi1", "RD"), 0x11: ("mthi1", "RS"), 0x12: ("mflo1", "RD"), 0x13: ("mtlo1", "RS"),
    0x18: ("mult1", "R3"), 0x19: ("multu1", "R3"), 0x1A: ("div1", "RSRT"), 0x1B: ("divu1", "RSRT"),
    0x20: ("madd1", "R3"), 0x21: ("maddu1", "R3"),
    0x34: ("psllh", "SH"), 0x36: ("psrlh", "SH"), 0x37: ("psrah", "SH"),
    0x3C: ("psllw", "SH"), 0x3E: ("psrlw", "SH"), 0x3F: ("psraw", "SH"),
}
MMI0 = {0x00: "paddw", 0x01: "psubw", 0x02: "pcgtw", 0x03: "pmaxw", 0x04: "paddh", 0x05: "psubh",
        0x06: "pcgth", 0x07: "pmaxh", 0x08: "paddb", 0x09: "psubb", 0x0A: "pcgtb", 0x10: "paddsw",
        0x11: "psubsw", 0x12: "pextlw", 0x13: "ppacw", 0x14: "paddsh", 0x15: "psubsh", 0x16: "pextlh",
        0x17: "ppach", 0x18: "paddsb", 0x19: "psubsb", 0x1A: "pextlb", 0x1B: "ppacb", 0x1E: "pext5",
        0x1F: "ppac5"}
MMI1 = {0x01: "pabsw", 0x02: "pceqw", 0x03: "pminw", 0x04: "padsbh", 0x05: "pabsh", 0x06: "pceqh",
        0x07: "pminh", 0x0A: "pceqb", 0x10: "padduw", 0x11: "psubuw", 0x12: "pextuw", 0x14: "padduh",
        0x15: "psubuh", 0x16: "pextuh", 0x18: "paddub", 0x19: "psubub", 0x1A: "pextub", 0x1B: "qfsrv"}
MMI2 = {0x00: "pmaddw", 0x02: "psllvw", 0x03: "psrlvw", 0x04: "pmsubw", 0x08: "pmfhi", 0x09: "pmflo",
        0x0A: "pinth", 0x0C: "pmultw", 0x0D: "pdivw", 0x0E: "pcpyld", 0x10: "pmaddh", 0x11: "phmadh",
        0x12: "pand", 0x13: "pxor", 0x14: "pmsubh", 0x15: "phmsbh", 0x1A: "pexeh", 0x1B: "prevh",
        0x1C: "pmulth", 0x1D: "pdivbw", 0x1E: "pexew", 0x1F: "prot3w"}
MMI3 = {0x00: "pmadduw", 0x03: "psravw", 0x08: "pmthi", 0x09: "pmtlo", 0x0A: "pinteh", 0x0C: "pmultuw",
        0x0D: "pdivuw", 0x0E: "pcpyud", 0x12: "por", 0x13: "pnor", 0x1A: "pexch", 0x1B: "pcpyh",
        0x1E: "pexcw"}
PMFHL = {0: "pmfhl.lw", 1: "pmfhl.uw", 2: "pmfhl.slw", 3: "pmfhl.lh", 4: "pmfhl.sh"}

# COP1 fmt S: funct -> (name, operand shape)
FPU_S = {
    0x00: ("add.s", "DST"), 0x01: ("sub.s", "DST"), 0x02: ("mul.s", "DST"), 0x03: ("div.s", "DST"),
    0x04: ("sqrt.s", "DT"), 0x05: ("abs.s", "DS"), 0x06: ("mov.s", "DS"), 0x07: ("neg.s", "DS"),
    0x16: ("rsqrt.s", "DST"), 0x18: ("adda.s", "ST"), 0x19: ("suba.s", "ST"), 0x1A: ("mula.s", "ST"),
    0x1C: ("madd.s", "DST"), 0x1D: ("msub.s", "DST"), 0x1E: ("madda.s", "ST"), 0x1F: ("msuba.s", "ST"),
    0x24: ("cvt.w.s", "DS"), 0x28: ("max.s", "DST"), 0x29: ("min.s", "DST"),
    0x30: ("c.f.s", "ST"), 0x32: ("c.eq.s", "ST"), 0x34: ("c.lt.s", "ST"), 0x36: ("c.le.s", "ST"),
}

BC = "xyzw"
# COP2 macro special1 (funct < 0x3C)
VU_S1 = {}
for i, base in enumerate(("vadd", "vsub", "vmadd", "vmsub", "vmax", "vmini", "vmul")):
    for b in range(4):
        VU_S1[i * 4 + b] = base + BC[b]
VU_S1.update({0x1C: "vmulq", 0x1D: "vmaxi", 0x1E: "vmuli", 0x1F: "vminii", 0x20: "vaddq", 0x21: "vmaddq",
              0x22: "vaddi", 0x23: "vmaddi", 0x24: "vsubq", 0x25: "vmsubq", 0x26: "vsubi", 0x27: "vmsubi",
              0x28: "vadd", 0x29: "vmadd", 0x2A: "vmul", 0x2B: "vmax", 0x2C: "vsub", 0x2D: "vmsub",
              0x2E: "vopmsub", 0x2F: "vmini", 0x30: "viadd", 0x31: "visub", 0x32: "viaddi",
              0x34: "viand", 0x35: "vior"})
VU_S1_SKIP = {0x38: "vcallms (VU0 microprogram)", 0x39: "vcallmsr (VU0 microprogram)"}
# COP2 macro special2: op = (sa << 2) | (funct & 3)
VU_S2 = {}
for i, base in enumerate(("vadda", "vsuba", "vmadda", "vmsuba")):
    for b in range(4):
        VU_S2[i * 4 + b] = base + BC[b]
for b in range(4):
    VU_S2[0x18 + b] = "vmula" + BC[b]
VU_S2.update({0x10: "vitof0", 0x11: "vitof4", 0x12: "vitof12", 0x13: "vitof15", 0x14: "vftoi0",
              0x15: "vftoi4", 0x16: "vftoi12", 0x17: "vftoi15", 0x1C: "vmulaq", 0x1D: "vabs",
              0x1E: "vmulai", 0x1F: "vclipw", 0x20: "vaddaq", 0x21: "vmaddaq", 0x22: "vaddai",
              0x23: "vmaddai", 0x24: "vsubaq", 0x25: "vmsubaq", 0x26: "vsubai", 0x27: "vmsubai",
              0x28: "vadda", 0x29: "vmadda", 0x2A: "vmula", 0x2C: "vsuba", 0x2D: "vmsuba",
              0x2E: "vopmula", 0x30: "vmove", 0x31: "vmr32", 0x38: "vdiv", 0x39: "vsqrt",
              0x3A: "vrsqrt", 0x3C: "vmtir", 0x3D: "vmfir", 0x40: "vrnext", 0x41: "vrget",
              0x42: "vrinit", 0x43: "vrxor"})
VU_S2_SKIP = {0x2F: "vnop", 0x3B: "vwaitq", 0x34: "vlqi (VU0 memory)", 0x35: "vsqi (VU0 memory)",
              0x36: "vlqd (VU0 memory)", 0x37: "vsqd (VU0 memory)", 0x3E: "vilwr (VU0 memory)",
              0x3F: "viswr (VU0 memory)"}
Q_WRITERS = {"vdiv", "vsqrt", "vrsqrt"}
# VU0 control registers a test may load (ctc2) and store (cfc2); 17 = MAC is read-only
VI_LOAD = list(range(1, 16)) + [16, 18, 20, 21, 22]
VI_STORE = list(range(1, 16)) + [16, 17, 18, 20, 21, 22]
CTRL_NAMES = {16: "STATUS", 17: "MAC", 18: "CLIP", 20: "R", 21: "I", 22: "Q"}


@dataclass
class Op:
    cls: str
    groups: str   # subset of "GFV"
    text: str     # listing
    shape: tuple  # variety key: words of one class with different shapes are all worth testing


def _rf(w):
    return (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63


def _gpr_text(name, kind, rs, rt, rd, sa):
    g = GPRN
    return {"R3": f"{name} {g[rd]}, {g[rs]}, {g[rt]}", "SH": f"{name} {g[rd]}, {g[rt]}, {sa}",
            "SHV": f"{name} {g[rd]}, {g[rt]}, {g[rs]}", "RD": f"{name} {g[rd]}", "RS": f"{name} {g[rs]}",
            "RSRT": f"{name} {g[rs]}, {g[rt]}", "RDRS": f"{name} {g[rd]}, {g[rs]}"}[kind]


def _gpr_op(name, kind, rs, rt, rd, sa):
    used = {"R3": (rs, rt, rd), "SH": (rt, rd), "SHV": (rs, rt, rd), "RD": (rd,), "RS": (rs,),
            "RSRT": (rs, rt), "RDRS": (rd, rs)}[kind]
    if K0 in used or K1 in used:
        return "uses k0/k1 (reserved)"
    shape = (rs == rt, rd == rs, rd == rt, rd == 0, sa if kind == "SH" else -1)
    return Op(name, "G", _gpr_text(name, kind, rs, rt, rd, sa), shape)


def classify(w):
    """-> Op, or a str giving the skip reason."""
    if w == 0:
        return "nop"
    op = w >> 26
    rs, rt, rd, sa, fn = _rf(w)
    if op == 0x00:
        if fn in SPECIAL:
            return _gpr_op(*SPECIAL[fn], rs, rt, rd, sa)
        return SPECIAL_SKIP.get(fn, "trap/unknown SPECIAL")
    if op == 0x01:
        if rt in REGIMM:
            if rs in (K0, K1):
                return "uses k0/k1 (reserved)"
            return Op(REGIMM[rt], "G", f"{REGIMM[rt]} {GPRN[rs]}, 0x{w & 0xFFFF:x}", (w & 0xFFFF,))
        return "branch/trap (REGIMM)"
    if op in IMM:
        name = IMM[op]
        if rt in (K0, K1) or (name != "lui" and rs in (K0, K1)):
            return "uses k0/k1 (reserved)"
        imm = w & 0xFFFF
        text = f"lui {GPRN[rt]}, 0x{imm:x}" if name == "lui" else f"{name} {GPRN[rt]}, {GPRN[rs]}, 0x{imm:x}"
        return Op(name, "G", text, (rs == rt, rt == 0, imm >= 0x8000))
    if op in (0x08, 0x18):
        return "addi/daddi traps on overflow"
    if op == 0x1C:
        if fn == 0x08 or fn == 0x09 or fn == 0x28 or fn == 0x29:
            table = {0x08: MMI0, 0x28: MMI1, 0x09: MMI2, 0x29: MMI3}[fn]
            if sa not in table:
                return "unknown MMI"
            name = table[sa]
            kind = {"pmfhi": "RD", "pmflo": "RD", "pmthi": "RS", "pmtlo": "RS"}.get(name, "R3")
            if name in ("pexeh", "prevh", "pexew", "prot3w", "pexch", "pcpyh", "pexcw", "pabsw", "pabsh",
                        "pext5", "ppac5"):
                kind = "RDRT"
            if name in ("pdivw", "pdivuw", "pdivbw"):
                kind = "RSRT"
            used = {"R3": (rs, rt, rd), "RD": (rd,), "RS": (rs,), "RDRT": (rd, rt), "RSRT": (rs, rt)}[kind]
            if K0 in used or K1 in used:
                return "uses k0/k1 (reserved)"
            text = {"RDRT": f"{name} {GPRN[rd]}, {GPRN[rt]}"}.get(kind) or _gpr_text(name, kind, rs, rt, rd, sa)
            return Op(name, "G", text, (rs == rt, rd == rs, rd == rt, rd == 0))
        if fn == 0x30:
            if sa not in PMFHL or rd in (K0, K1):
                return "pmfhl bad/k0k1"
            return Op(PMFHL[sa], "G", f"{PMFHL[sa]} {GPRN[rd]}", (rd == 0,))
        if fn == 0x31:
            if sa != 0 or rs in (K0, K1):
                return "pmthl bad/k0k1"
            return Op("pmthl.lw", "G", f"pmthl.lw {GPRN[rs]}", ())
        if fn in MMI:
            return _gpr_op(*MMI[fn], rs, rt, rd, sa)
        return "unknown MMI"
    if op == 0x11:  # COP1
        if rs in (0, 2, 4, 6):
            name = {0: "mfc1", 2: "cfc1", 4: "mtc1", 6: "ctc1"}[rs]
            if rt in (K0, K1):
                return "uses k0/k1 (reserved)"
            if name in ("cfc1", "ctc1") and rd not in (0, 31):
                return "cfc1/ctc1 bad fcr"
            if name == "ctc1" and rd == 0:
                return "ctc1 to fcr0 (read only)"
            reg = f"fcr{rd}" if name in ("cfc1", "ctc1") else f"$f{rd}"
            return Op(name, "GF", f"{name} {GPRN[rt]}, {reg}", (rd,) if name in ("cfc1", "ctc1") else (rt == 0,))
        if rs == 0x08:
            return "branch (bc1)"
        if rs == 0x10 and fn in FPU_S:
            name, kind = FPU_S[fn]
            fs, ft, fd = rd, rt, sa
            text = {"DST": f"{name} $f{fd}, $f{fs}, $f{ft}", "DT": f"{name} $f{fd}, $f{ft}",
                    "DS": f"{name} $f{fd}, $f{fs}", "ST": f"{name} $f{fs}, $f{ft}"}[kind]
            return Op(name, "F", text, (fd == fs, fd == ft, fs == ft))
        if rs == 0x14 and fn == 0x20:
            return Op("cvt.s.w", "F", f"cvt.s.w $f{sa}, $f{rd}", (sa == rd,))
        return "unknown COP1"
    if op == 0x12:  # COP2 (VU0 macro)
        if rs in (1, 2, 5, 6):
            name = {1: "qmfc2", 2: "cfc2", 5: "qmtc2", 6: "ctc2"}[rs] + (".i" if w & 1 else "")
            if rt in (K0, K1):
                return "uses k0/k1 (reserved)"
            if rs == 2 and rd not in VI_STORE:
                return "cfc2 of a VU0 system register"
            if rs == 6 and rd not in VI_LOAD:
                return "ctc2 to a VU0 system register"
            reg = (f"$vi{rd}" if rd < 16 else CTRL_NAMES[rd]) if rs in (2, 6) else f"$vf{rd}"
            return Op(name, "GV", f"{name} {GPRN[rt]}, {reg}", (rd < 16, rd == 0, rt == 0))
        if rs == 0x08:
            return "branch (bc2)"
        if rs >= 0x10:
            dest = (w >> 21) & 15
            ft, fs, fd = rt, rd, sa
            dm = "".join(c for c, bit in zip("xyzw", (8, 4, 2, 1)) if dest & bit)
            if fn < 0x3C:
                if fn in VU_S1_SKIP:
                    return VU_S1_SKIP[fn]
                if fn not in VU_S1:
                    return "unknown COP2 special1"
                name = VU_S1[fn]
                if name in ("viadd", "visub", "viand", "vior"):
                    text = f"{name} $vi{fd}, $vi{fs}, $vi{ft}"
                elif name == "viaddi":
                    imm = sa if sa < 16 else sa - 32
                    text = f"viaddi $vi{ft}, $vi{fs}, {imm}"
                else:
                    text = f"{name}.{dm} $vf{fd}, $vf{fs}, $vf{ft}"
                return Op(name, "V", text, (dest, fd == fs, fd == ft, fs == ft))
            op2 = (sa << 2) | (fn & 3)
            if op2 in VU_S2_SKIP:
                return VU_S2_SKIP[op2]
            if op2 not in VU_S2:
                return "unknown COP2 special2"
            name = VU_S2[op2]
            fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
            if name in ("vdiv", "vsqrt", "vrsqrt"):
                text = f"{name} Q, $vf{fs}.{BC[fsf]}, $vf{ft}.{BC[ftf]}"
                shape = (fsf, ftf, fs == ft)
            elif name in ("vmtir",):
                text = f"vmtir $vi{ft}, $vf{fs}.{BC[fsf]}"
                shape = (fsf,)
            elif name in ("vmfir",):
                text = f"vmfir.{dm} $vf{ft}, $vi{fs}"
                shape = (dest,)
            elif name in ("vrinit", "vrxor"):
                text = f"{name} R, $vf{fs}.{BC[fsf]}"
                shape = (fsf,)
            elif name in ("vrnext", "vrget"):
                text = f"{name}.{dm} $vf{ft}, R"
                shape = (dest,)
            else:
                text = f"{name}.{dm} $vf{ft}, $vf{fs}"
                shape = (dest, fs == ft)
            return Op(name, "V", text, shape)
        return "unknown COP2"
    return "load/store/branch/other"


# ---------------------------------------------------------------- encoder
def itype(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


LQ, SQ, LW, SW, LWC1, SWC1, LQC2, SQC2, LD, SD = 0x1E, 0x1F, 0x23, 0x2B, 0x31, 0x39, 0x36, 0x3E, 0x37, 0x3F
NOP = 0
JR_RA = 0x03E00008
DI = 0x42000039
VWAITQ = 0x4A0003BF


def lui(rt, imm):
    return itype(0x0F, 0, rt, imm)


def ori(rt, rs, imm):
    return itype(0x0D, rs, rt, imm)


def addiu(rt, rs, imm):
    return itype(0x09, rs, rt, imm)


def beq(rs, rt, off):
    return itype(0x04, rs, rt, off)


def sll(rd, rt, sa):
    return (rt << 16) | (rd << 11) | (sa << 6)


def addu(rd, rs, rt):
    return (rs << 21) | (rt << 16) | (rd << 11) | 0x21


def jalr(rs):
    return (rs << 21) | (31 << 11) | 0x09


def mtsa(rs):
    return (rs << 21) | 0x29


def mfsa(rd):
    return (rd << 11) | 0x28


def mthi(rs):
    return (rs << 21) | 0x11


def mtlo(rs):
    return (rs << 21) | 0x13


def mfhi(rd):
    return (rd << 11) | 0x10


def mflo(rd):
    return (rd << 11) | 0x12


def mthi1(rs):
    return (0x1C << 26) | (rs << 21) | 0x11


def mtlo1(rs):
    return (0x1C << 26) | (rs << 21) | 0x13


def mfhi1(rd):
    return (0x1C << 26) | (rd << 11) | 0x10


def mflo1(rd):
    return (0x1C << 26) | (rd << 11) | 0x12


def pmthi(rs):
    return (0x1C << 26) | (rs << 21) | (0x08 << 6) | 0x29


def pmtlo(rs):
    return (0x1C << 26) | (rs << 21) | (0x09 << 6) | 0x29


def pmfhi(rd):
    return (0x1C << 26) | (rd << 11) | (0x08 << 6) | 0x09


def pmflo(rd):
    return (0x1C << 26) | (rd << 11) | (0x09 << 6) | 0x09


def mtc1(rt, fs):
    return (0x11 << 26) | (4 << 21) | (rt << 16) | (fs << 11)


def ctc1(rt, fs):
    return (0x11 << 26) | (6 << 21) | (rt << 16) | (fs << 11)


def cfc1(rt, fs):
    return (0x11 << 26) | (2 << 21) | (rt << 16) | (fs << 11)


def mula_s(fs, ft):
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | 0x1A


def madd_s(fd, fs, ft):
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x1C


def ctc2(rt, rd):
    return (0x12 << 26) | (6 << 21) | (rt << 16) | (rd << 11)


def cfc2(rt, rd):
    return (0x12 << 26) | (2 << 21) | (rt << 16) | (rd << 11)


def vmulaw(dest, fs, ft):  # ACC = fs * ft.w
    return (0x12 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((0x1B >> 2) << 6) | 0x3C | (0x1B & 3)


def vmaddx(dest, fd, fs, ft):  # fd = ACC + fs * ft.x
    return (0x12 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x08
