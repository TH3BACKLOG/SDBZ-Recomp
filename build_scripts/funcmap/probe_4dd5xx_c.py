"""Final check before editing the map: does each of the four ranges TERMINATE?

Established by probe_b: the pointer table at 0x4e6c90 has 86 targets; 82 are
already defined functions in the merged map, and coverage begins exactly at the
5th target (0x4dd630). Only the first four are missing, and the hole
0x4dd500-0x4dd630 is bounded on both sides. Each target is preceded by a
padding nop, which is a real function boundary.

The earlier `jr $ra` scan found nothing because it ran PAST each function into
its neighbours. If these are genuine functions, each must end with a return or
a tail-call jump followed by padding, strictly inside its range.

If any range does NOT terminate cleanly, do not add it -- report instead.
"""
import struct

ELF = r"F:\SDBZ Recomp\ELF\SLUS_214.42"
RANGES = [(0x4DD500, 0x4DD570), (0x4DD570, 0x4DD5B0),
          (0x4DD5B0, 0x4DD5E0), (0x4DD5E0, 0x4DD630)]


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
    e_phes = struct.unpack_from("<H", data, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phes
        t, o, v, _p, fs, _m, _fl = struct.unpack_from("<IIIIIII", data, off)
        if t == 1:
            segs.append((v, data[o:o + fs]))
    return segs


def word_at(segs, a):
    for b, d in segs:
        if b <= a < b + len(d) - 3:
            return struct.unpack_from("<I", d, a - b)[0]
    return None


def main():
    segs = load(ELF)
    for start, stop in RANGES:
        print(f"--- 0x{start:08x} .. 0x{stop:08x} (0x{stop-start:x}) ---")
        last_real = None
        ret = None
        for a in range(start, stop, 4):
            w = word_at(segs, a)
            if w != 0:
                last_real = (a, w)
            if w == 0x03E00008:
                ret = a
        if ret is not None:
            print(f"  jr $ra at 0x{ret:08x}  -> real end 0x{ret+8:08x}")
        else:
            a, w = last_real
            op = w >> 26
            fn = w & 0x3F
            if op == 2:
                kind = f"j 0x{((w & 0x3FFFFFF) << 2):08x}  (TAIL CALL)"
            elif op == 0 and fn == 0x08:
                kind = "jr (indirect TAIL CALL)"
            elif op == 3:
                kind = f"jal 0x{((w & 0x3FFFFFF) << 2):08x}"
            else:
                kind = "no return/jump found -- SUSPECT"
            print(f"  no jr $ra; last non-nop 0x{a:08x}: 0x{w:08x}  {kind}")
        tail_nops = sum(1 for a in range(last_real[0] + 4, stop, 4)
                        if word_at(segs, a) == 0)
        print(f"  trailing padding nops before next entry: {tail_nops}")
        print()


main()
