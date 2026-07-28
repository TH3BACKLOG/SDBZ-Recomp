#!/usr/bin/env python3
"""Disassemble MIPS R5900 code straight out of the game ELF.

Replaces the elf_disasm.py / mips_r5900_disassembler.py whose source was lost
(only the .pyc survived). Self-contained: no capstone, no external deps.

    python mips_r5900_disassembler.py <elf> <addr> [count]
    python mips_r5900_disassembler.py <elf> <addr> --func     # to next jr $ra

Addresses are guest virtual addresses (e.g. 0x104bf0). Branch/jump targets are
resolved and printed, and the '-> loop' marker flags a backward branch whose
target is inside the range being printed (i.e. a spin candidate).
"""
import struct
import sys

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn", 0x0C: "syscall",
    0x0D: "break", 0x0F: "sync", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo",
    0x13: "mtlo", 0x14: "dsllv", 0x16: "dsrlv", 0x17: "dsrav", 0x18: "mult",
    0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x1C: "dmult", 0x1D: "dmultu",
    0x1E: "ddiv", 0x1F: "ddivu", 0x20: "add", 0x21: "addu", 0x22: "sub",
    0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor",
    0x2A: "slt", 0x2B: "sltu", 0x2C: "dadd", 0x2D: "daddu", 0x2E: "dsub",
    0x2F: "dsubu", 0x38: "dsll", 0x3A: "dsrl", 0x3B: "dsra", 0x3C: "dsll32",
    0x3E: "dsrl32", 0x3F: "dsra32",
}

REGIMM = {0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl",
          0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall"}

OPCODE = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi",
    0x0D: "ori", 0x0E: "xori", 0x0F: "lui", 0x14: "beql", 0x15: "bnel",
    0x16: "blezl", 0x17: "bgtzl", 0x18: "daddi", 0x19: "daddiu", 0x1A: "ldl",
    0x1B: "ldr", 0x1C: "mmi", 0x1E: "lq", 0x1F: "sq", 0x20: "lb", 0x21: "lh",
    0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x27: "lwu",
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2C: "sdl", 0x2D: "sdr",
    0x2E: "swr", 0x2F: "cache", 0x31: "lwc1", 0x33: "pref", 0x36: "lqc2",
    0x37: "ld", 0x39: "swc1", 0x3E: "sqc2", 0x3F: "sd",
}

LOADSTORE = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "lwu", "sb", "sh",
             "swl", "sw", "sdl", "sdr", "swr", "ld", "sd", "lq", "sq",
             "lwc1", "swc1", "lqc2", "sqc2", "ldl", "ldr", "cache", "pref"}
BRANCH2 = {"beq", "bne", "beql", "bnel"}
BRANCH1 = {"blez", "bgtz", "blezl", "bgtzl"}
IMM_SIGNED = {"addi", "addiu", "slti", "sltiu", "daddi", "daddiu"}


def load_elf(path):
    """Return (segments, entry) where segments = [(vaddr, bytes)]."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: not an ELF")
    entry, phoff = struct.unpack_from("<II", data, 0x18)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    segs = []
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz = struct.unpack_from("<IIIII", data, off)
        if p_type == 1 and p_filesz:  # PT_LOAD
            segs.append((p_vaddr, data[p_offset:p_offset + p_filesz]))
    return segs, entry


def read_word(segs, addr):
    for vaddr, blob in segs:
        rel = addr - vaddr
        if 0 <= rel <= len(blob) - 4:
            return struct.unpack_from("<I", blob, rel)[0]
    return None


def decode(word, pc):
    """Return (text, branch_target_or_None, is_return)."""
    op = word >> 26
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    sa, funct = (word >> 6) & 31, word & 63
    imm = word & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm

    if word == 0:
        return "nop", None, False

    if op == 0:
        name = SPECIAL.get(funct)
        if name is None:
            return f".word 0x{word:08x}", None, False
        if name == "jr":
            return f"jr      ${GPR[rs]}", None, rs == 31
        if name == "jalr":
            return f"jalr    ${GPR[rd]}, ${GPR[rs]}", None, False
        if name in ("sll", "srl", "sra", "dsll", "dsrl", "dsra",
                    "dsll32", "dsrl32", "dsra32"):
            return f"{name:<7} ${GPR[rd]}, ${GPR[rt]}, {sa}", None, False
        if name in ("mfhi", "mflo"):
            return f"{name:<7} ${GPR[rd]}", None, False
        if name in ("mthi", "mtlo"):
            return f"{name:<7} ${GPR[rs]}", None, False
        if name in ("mult", "multu", "div", "divu", "dmult", "dmultu",
                    "ddiv", "ddivu"):
            return f"{name:<7} ${GPR[rs]}, ${GPR[rt]}", None, False
        if name in ("syscall", "break", "sync"):
            return name, None, False
        return f"{name:<7} ${GPR[rd]}, ${GPR[rs]}, ${GPR[rt]}", None, False

    if op == 1:
        name = REGIMM.get(rt, f"regimm{rt:02x}")
        tgt = pc + 4 + (simm << 2)
        return f"{name:<7} ${GPR[rs]}, 0x{tgt:x}", tgt, False

    if op in (0x10, 0x11, 0x12):  # cop0/cop1/cop2
        cop = op & 3
        if rs == 0:
            return f"mfc{cop}    ${GPR[rt]}, ${rd}", None, False
        if rs == 4:
            return f"mtc{cop}    ${GPR[rt]}, ${rd}", None, False
        if rs == 8:  # bc1
            tgt = pc + 4 + (simm << 2)
            name = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}.get(rt & 3, "bc1?")
            return f"{name:<7} 0x{tgt:x}", tgt, False
        return f"cop{cop}    0x{word & 0x1FFFFFF:07x}", None, False

    name = OPCODE.get(op)
    if name is None:
        return f".word 0x{word:08x}", None, False

    if name in ("j", "jal"):
        tgt = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
        return f"{name:<7} 0x{tgt:x}", tgt, False
    if name in BRANCH2:
        tgt = pc + 4 + (simm << 2)
        return f"{name:<7} ${GPR[rs]}, ${GPR[rt]}, 0x{tgt:x}", tgt, False
    if name in BRANCH1:
        tgt = pc + 4 + (simm << 2)
        return f"{name:<7} ${GPR[rs]}, 0x{tgt:x}", tgt, False
    if name == "lui":
        return f"lui     ${GPR[rt]}, 0x{imm:x}", None, False
    if name in LOADSTORE:
        reg = f"$f{rt}" if name in ("lwc1", "swc1") else f"${GPR[rt]}"
        return f"{name:<7} {reg}, {simm}(${GPR[rs]})", None, False
    if name == "mmi":
        return f"mmi.{funct:02x} ${GPR[rd]}, ${GPR[rs]}, ${GPR[rt]}", None, False
    shown = simm if name in IMM_SIGNED else imm
    return f"{name:<7} ${GPR[rt]}, ${GPR[rs]}, 0x{shown & 0xFFFFFFFF:x}", None, False


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    elf, addr = sys.argv[1], int(sys.argv[2], 0)
    to_end = "--func" in sys.argv[3:]
    count = 64
    for a in sys.argv[3:]:
        if not a.startswith("-"):
            count = int(a, 0)
    limit = 4096 if to_end else count

    segs, _entry = load_elf(elf)
    lines, pending_end = [], None
    pc = addr
    for i in range(limit):
        word = read_word(segs, pc)
        if word is None:
            lines.append((pc, "<unmapped>", None))
            break
        text, tgt, is_ret = decode(word, pc)
        lines.append((pc, text, tgt))
        if is_ret and to_end and pending_end is None:
            pending_end = pc + 8  # include the delay slot
        if pending_end is not None and pc + 4 >= pending_end:
            break
        pc += 4

    lo, hi = addr, pc
    for at, text, tgt in lines:
        mark = ""
        if tgt is not None and lo <= tgt <= hi:
            mark = "   <- loop" if tgt <= at else "   -> fwd"
        print(f"0x{at:08x}  {text}{mark}")


if __name__ == "__main__":
    main()
