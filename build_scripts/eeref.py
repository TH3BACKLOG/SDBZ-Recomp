#!/usr/bin/env python3
"""Static cross-reference engine for the EE image -- answer "who reaches X"
without a build and without a run.

WHY THIS EXISTS. Stage 5.17 spent runs 74-81 narrowing the movie data-supply
path to sub_168320 @ 0x168320, one probe field per multi-hour build+run cycle.
Four minutes of scanning this ELF showed 0x168320 has no jal, no data pointer
and no lui/addiu materialization anywhere in the loaded image -- it is CRI
library code that nothing in the game can call. Eight cycles were spent proving
that dead code stays dead. Every question of the form "who calls this", "who
writes offset N", "does this have a dispatch slot" is answerable here in
seconds, and none of them should ever cost a run again.

    eeref.py refs 0x168320                # jal / pointer / lui+lo / $gp refs
    eeref.py up 0x167B68 --depth 4        # reverse call graph
    eeref.py down 0x14C0D0 --depth 3      # forward closure
    eeref.py field 14000                  # readers and writers of a struct offset
    eeref.py field 14000:14012            # ... an offset range
    eeref.py vtable 0x4BF780 --count 16   # dump and name a vtable

KNOWN LIMIT, and it is the reason runtime probes are not dead: this sees the
STATIC image only. A dispatch table assembled at runtime -- entries computed,
copied, or relocated after load -- is invisible here. Zero references from this
tool means "nothing in the linked image refers to it", not "nothing ever will".
When that distinction matters, scan RAM at runtime instead.

SECOND LIMIT, specific to `field`: the compiler frequently folds a large struct
offset into the base register (`addiu $v0,$a0,14000` then `sw $x,4($v0)`), so a
naive scan for imm==14004 finds nothing and reports a false negative. This tool
tracks addiu chains per register and reports the effective offset. Hits found
that way are marked `fold` -- if a `field` query returns only `direct` hits for
an offset you expect to be folded, suspect the tracker before the game.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mips_r5900_disassembler import load_elf  # noqa: E402
from find_dispatch_holes import (  # noqa: E402
    DEFAULT_CSV,
    DEFAULT_ELF,
    DEFAULT_OVERRIDES,
    DEFAULT_RECOVERED,
    DEFAULT_REG,
    load_overrides,
    load_ranges,
    load_recovered,
    load_registered,
    make_owner_lookup,
)

# Same window find_dispatch_holes.py uses. Anything outside it in a decoded
# field is noise, not a code reference.
CODE_LO, CODE_HI = 0x00100000, 0x00500000

REGS = [
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
]

# Memory ops we care about for `field`. Everything that takes imm16(base).
LOADS = {
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu",
    0x25: "lhu", 0x26: "lwr", 0x27: "lwu", 0x37: "ld",
    0x1A: "ldl", 0x1B: "ldr", 0x1E: "lq",
    0x31: "lwc1", 0x35: "ldc1",
}
STORES = {
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x3F: "sd", 0x2C: "sdl", 0x2D: "sdr", 0x1F: "sq",
    0x39: "swc1", 0x3D: "sdc1",
}
# sdl/sdr matter more than they look. The one 64-bit store this project has
# actually needed to find -- *(u64 *)(obj + 14004) inside sub_168320 -- is
# emitted as `sdl $v0,14011($s0)` + `sdr $v0,14004($s0)` because +14004 is not
# 8-aligned. A table without them reports "nothing writes that field", which is
# the exact false negative this tool exists to stop producing. Note the sdl
# offset names the LAST byte of the store, so a range query has to be wide
# enough to catch it.

# Ops that write $rt with a value we cannot track. Anything not listed and not
# an addiu chain link clears the register's tracked offset.
WRITES_RT = set(LOADS) | set(
    (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19)
)


def seg_iter(segs):
    """Yield (addr, word) over every 4-byte-aligned word of every PT_LOAD."""
    for vaddr, blob in segs:
        n = len(blob) & ~3
        for off in range(0, n, 4):
            yield vaddr + off, struct.unpack_from("<I", blob, off)[0]


def read_word(segs, addr):
    for vaddr, blob in segs:
        rel = addr - vaddr
        if 0 <= rel <= len(blob) - 4:
            return struct.unpack_from("<I", blob, rel)[0]
    return None


def find_gp(segs):
    """Recover the value of $gp by decoding crt0.

    2026-08-23 -- this function exists because `refs 0x500728` said
    "UNREACHABLE in the static image" about a global that six instructions
    touch. PS2 SDK code addresses the small-data area as imm16($gp), so a
    lui-only index is structurally blind to an entire class of globals -- and
    it reports that blindness as a confident zero, which is the exact false
    negative this tool exists to kill ([[feedback_capped_probes_false_negatives]]).
    Stage 5.17 spent two weeks concluding 0x500724/0x500728 were "not globals,
    fields off a register-held base" on the strength of that zero.

    The ELF has no symtab, so $gp is not readable as a symbol. It is set up
    once in crt0 and never reassigned:

        0x00100198  lui   $a0, 0x50
        0x001001ac  addiu $a0, $a0, 0x3070
        0x001001c0  or    $gp, $a0, $zero     ==> $gp = 0x503070

    So: track lui/addiu/ori constants, and return the first value that lands
    in $gp -- either written directly or moved in from a tracked register.
    """
    val = {}
    for addr, w in seg_iter(segs):
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31

        if op == 0x0F:  # lui
            val[rt] = ((w & 0xFFFF) << 16) & 0xFFFFFFFF
            continue

        if op == 0x09 or op == 0x19 or op == 0x0D:  # addiu / daddiu / ori
            if rs in val:
                imm = w & 0xFFFF
                if op != 0x0D and (imm & 0x8000):
                    imm -= 0x10000
                val[rt] = (val[rs] + imm) & 0xFFFFFFFF
                if rt == 28:
                    return val[rt], addr
            else:
                val.pop(rt, None)
            continue

        if op == 0:
            funct = w & 0x3F
            rd = (w >> 11) & 31
            # addu / or / daddu  rd, rs, $zero -- the SDK's register move
            if funct in (0x21, 0x25, 0x2D) and rt == 0 and rs in val:
                val[rd] = val[rs]
                if rd == 28:
                    return val[rd], addr
            elif rd:
                val.pop(rd, None)
            continue

        if op in WRITES_RT:
            val.pop(rt, None)
    return None, None


class Index(object):
    """One pass over the image, three reference maps.

    Built eagerly because a full pass is ~1M iterations and costs under two
    seconds -- cheaper than deciding which map a query needs.
    """

    def __init__(self, segs, gp=None):
        self.segs = segs
        self.gp = gp
        self.calls = {}   # target -> [(site, "jal"|"j")]
        self.ptrs = {}    # value  -> [addr of the word]
        self.imms = {}    # value  -> [addr of the low half of the lui pair]
        self.gps = {}     # EA     -> [(addr, mnemonic)]  for imm16($gp)
        self.callees = {}  # site-owner-agnostic: caller site -> target

        lui = {}
        for addr, w in seg_iter(segs):
            op = w >> 26

            # $gp-relative access. Must come first: these instructions are
            # invisible to every other map here, because the address never
            # exists as a lui-materialized constant anywhere in the image.
            if gp is not None and ((w >> 21) & 31) == 28:
                mn = LOADS.get(op) or STORES.get(op)
                if mn is None and op in (0x09, 0x19):
                    mn = "addiu" if op == 0x09 else "daddiu"
                if mn is not None:
                    gi = w & 0xFFFF
                    if gi & 0x8000:
                        gi -= 0x10000
                    self.gps.setdefault((gp + gi) & 0xFFFFFFFF, []).append((addr, mn))

            if op == 3 or op == 2:
                tgt = ((addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                self.calls.setdefault(tgt, []).append((addr, "jal" if op == 3 else "j"))
                self.callees[addr] = tgt
                continue

            if op == 0x0F:  # lui
                lui[(w >> 16) & 31] = (w & 0xFFFF, addr)
            elif op == 0x09 or op == 0x19 or op == 0x0D:  # addiu / daddiu / ori
                rs = (w >> 21) & 31
                rt = (w >> 16) & 31
                pair = lui.get(rs)
                if pair is not None:
                    hi, _ha = pair
                    imm = w & 0xFFFF
                    if op != 0x0D and (imm & 0x8000):
                        imm -= 0x10000
                    val = ((hi << 16) + imm) & 0xFFFFFFFF
                    # 2026-08-21 -- was `if CODE_LO <= val < CODE_HI`, and that
                    # made every DATA address report "UNREACHABLE in the static
                    # image", confidently and wrongly. It cost a detour on the
                    # movie-gate hunt: the callback class table at 0x54E960 is
                    # materialized six times inside 0x13Cxxx, and eeref said
                    # zero, because 0x54E960 sits above CODE_HI. A global, a
                    # vtable base and a work buffer are all legitimate things to
                    # ask `refs` about, and a range filter that silences them is
                    # exactly the false-negative shape this tool exists to kill
                    # ([[feedback_capped_probes_false_negatives]]). Record every
                    # materialized constant; callers can filter if they want to.
                    # daddiu (0x19) was missing outright -- same class of hole as
                    # the sdl/sdr gap in `field`.
                    self.imms.setdefault(val, []).append(addr)
                    if rt != rs:
                        lui.pop(rs, None)
                else:
                    lui.pop(rt, None)
            elif op in WRITES_RT:
                lui.pop((w >> 16) & 31, None)

            # A word that IS a code address, in a segment, is a candidate
            # pointer. Instructions can alias this by coincidence; the owner
            # column makes those obvious (a "pointer" inside a function body is
            # almost always an encoded instruction).
            if CODE_LO <= w < CODE_HI:
                self.ptrs.setdefault(w, []).append(addr)


def load_slots():
    """Addresses the runtime can actually dispatch to -- generated table plus
    game_overrides.cpp registrations plus the recovered-tail loop. This is the
    static form of hasFunction(), which until now cost a build and a run."""
    slots = load_registered(DEFAULT_REG) if os.path.exists(DEFAULT_REG) else set()
    slots |= load_overrides(DEFAULT_OVERRIDES)
    slots |= load_recovered(DEFAULT_RECOVERED)
    return slots


def name_of(owner, addr):
    row = owner(addr)
    if row is None:
        return "?"
    if row[0] == addr:
        return row[2]
    return "%s+0x%x" % (row[2], addr - row[0])


# ---------------------------------------------------------------------------
# subcommands


def cmd_refs(idx, owner, slots, args):
    addr = args.addr
    row = owner(addr)
    print("target 0x%06x  %s  slot=%s" % (
        addr,
        row[2] if row else "NOT IN FUNC MAP",
        "yes" if addr in slots else "NO -- no dispatch slot",
    ))
    if row and row[0] != addr:
        print("  note: not a func-map row start -- inside %s (folded body)" % row[2])

    calls = idx.calls.get(addr, [])
    ptrs = idx.ptrs.get(addr, [])
    imms = idx.imms.get(addr, [])
    gps = idx.gps.get(addr, [])
    for site, kind in calls:
        print("CALL 0x%06x  %-3s  in %s" % (site, kind, name_of(owner, site)))
    for site in ptrs:
        print("PTR  0x%06x  word  in %s" % (site, name_of(owner, site)))
    for site in imms:
        print("IMM  0x%06x  lui+lo  in %s" % (site, name_of(owner, site)))
    for site, mn in gps:
        print("GP   0x%06x  %-6s %d($gp)  in %s" % (
            site, mn, addr - idx.gp, name_of(owner, site)))
    print("total: call=%d ptr=%d imm=%d gp=%d" % (
        len(calls), len(ptrs), len(imms), len(gps)))
    if idx.gp is None:
        print("note: $gp not recovered -- imm16($gp) globals are NOT covered.")
    if not (calls or ptrs or imms or gps):
        print("UNREACHABLE in the static image -- nothing links to this address.")


def _walk_up(idx, owner, addr, depth, seen, indent):
    if depth == 0 or addr in seen:
        return
    seen.add(addr)
    parents = {}
    for site, kind in idx.calls.get(addr, []):
        row = owner(site)
        start = row[0] if row else site
        parents.setdefault(start, [0, kind])[0] += 1
    for start in sorted(parents):
        n = len(idx.calls.get(start, []))
        print("%s<- 0x%06x %-32s callers=%d" % (
            "  " * indent, start, name_of(owner, start), n))
        _walk_up(idx, owner, start, depth - 1, seen, indent + 1)


def cmd_up(idx, owner, slots, args):
    row = owner(args.addr)
    print("=== 0x%06x %s" % (args.addr, row[2] if row else "?"))
    _walk_up(idx, owner, args.addr, args.depth, set(), 1)
    ptrs = idx.ptrs.get(args.addr, [])
    if ptrs:
        print("(also referenced as a data word at %s -- vtable/callback reach)"
              % ", ".join("0x%x" % p for p in ptrs[:8]))
    if not idx.calls.get(args.addr) and not ptrs:
        print("(no callers and no pointers -- dead in the static image)")


def cmd_down(idx, owner, slots, args):
    frontier = [(args.addr, 0)]
    seen = set()
    while frontier:
        addr, d = frontier.pop(0)
        if addr in seen or d > args.depth:
            continue
        seen.add(addr)
        row = owner(addr)
        if row is None:
            continue
        print("%s0x%06x %-32s slot=%s" % (
            "  " * d, addr, row[2], "yes" if addr in slots else "NO"))
        for site in range(row[0], row[1], 4):
            tgt = idx.callees.get(site)
            if tgt is not None and CODE_LO <= tgt < CODE_HI and tgt not in seen:
                frontier.append((tgt, d + 1))
    print("reached %d functions" % len(seen))


def cmd_field(idx, owner, slots, args):
    lo, _, hi = args.offset.partition(":")
    lo = int(lo, 0)
    hi = int(hi, 0) if hi else lo
    print("offsets %d..%d (0x%x..0x%x)" % (lo, hi, lo, hi))

    # Per-register "reg = base + K" tracking, reset at every func-map boundary
    # so a chain never leaks across functions.
    track = {}
    cur_owner = None
    hits = 0
    for addr, w in seg_iter(idx.segs):
        row = owner(addr)
        if row is not cur_owner:
            cur_owner = row
            track = {}
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm

        if op == 0x09:  # addiu -- the fold we have to follow
            if rt == 0:
                continue
            base, k = track.get(rs, (rs, 0))
            track[rt] = (base, k + simm)
            continue

        kind = None
        if op in STORES:
            kind = "ST"
            mnem = STORES[op]
        elif op in LOADS:
            kind = "LD"
            mnem = LOADS[op]

        if kind is not None:
            base, k = track.get(rs, (rs, 0))
            eff = k + simm
            if lo <= eff <= hi:
                print("%s 0x%06x  %-4s off=+%-6d %-6s base=%s%s  in %s" % (
                    kind, addr, mnem, eff,
                    "fold" if k else "direct",
                    REGS[base],
                    "+%d" % k if k else "",
                    name_of(owner, addr)))
                hits += 1
            continue

        if op == 0 or op in WRITES_RT or op == 0x0F:
            track.pop(rd_of(w) if op == 0 else rt, None)
    print("total: %d" % hits)


def rd_of(w):
    return (w >> 11) & 31


def cmd_vtable(idx, owner, slots, args):
    for i in range(args.count):
        a = args.addr + 4 * i
        w = read_word(idx.segs, a)
        if w is None:
            print("[%2d] 0x%06x  <outside loaded image>" % (i, a))
            continue
        if not (CODE_LO <= w < CODE_HI):
            print("[%2d] 0x%06x  0x%08x  (not a code address)" % (i, a, w))
            continue
        row = owner(w)
        print("[%2d] 0x%06x  -> 0x%06x  %-32s slot=%s" % (
            i, a, w, row[2] if row else "NOT IN FUNC MAP",
            "yes" if w in slots else "NO"))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--csv", default=DEFAULT_CSV)
    ap.add_argument("--gp", type=lambda s: int(s, 0), default=None,
                    help="override the $gp base (default: decoded from crt0)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("refs", help="who calls / points at / materializes an address")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.set_defaults(fn=cmd_refs)

    p = sub.add_parser("up", help="reverse call graph")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--depth", type=int, default=4)
    p.set_defaults(fn=cmd_up)

    p = sub.add_parser("down", help="forward call closure")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--depth", type=int, default=2)
    p.set_defaults(fn=cmd_down)

    p = sub.add_parser("field", help="readers/writers of a struct offset")
    p.add_argument("offset", help="N or LO:HI")
    p.set_defaults(fn=cmd_field)

    p = sub.add_parser("vtable", help="dump and name a vtable")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--count", type=int, default=16)
    p.set_defaults(fn=cmd_vtable)

    args = ap.parse_args()
    segs, _entry = load_elf(args.elf)
    owner = make_owner_lookup(load_ranges(args.csv))
    gp = args.gp
    if gp is None:
        gp, gpsite = find_gp(segs)
        if gp is not None:
            sys.stderr.write("$gp = 0x%06x  (set at 0x%06x)\n" % (gp, gpsite))
        else:
            sys.stderr.write("$gp NOT RECOVERED -- imm16($gp) globals uncovered\n")
    idx = Index(segs, gp)
    args.fn(idx, owner, load_slots(), args)


if __name__ == "__main__":
    main()
