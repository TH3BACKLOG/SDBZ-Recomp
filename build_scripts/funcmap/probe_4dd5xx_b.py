"""Follow-up: are the 0x4dd5xx entries FUNCTIONS or SWITCH-CASE targets?

probe_4dd5xx.py found none of them own a `jr $ra` -- all four scan forward to
the SAME return at 0x004dd978, and two overlap each other. Separate functions
cannot share a single epilogue while overlapping. Two readings remain:

  (a) one function spanning ~0x4dd500-0x4dd980 with multiple entry points
  (b) a switch jump table whose cases fall through to a shared epilogue

They need different fixes. (a) = add ONE function to the map. (b) = entry
guards inside the owning function; adding four functions would be wrong and
would carve up a body the recompiler needs whole.

Distinguishing evidence:
  - the full extent of the pointer table (how many targets, are they sorted)
  - the instruction immediately BEFORE each entry: an unconditional
    branch/jump/return means a real boundary; anything else means fall-through
  - what IDA thinks owns the region
"""
import csv
import os
import struct

ELF = r"F:\SDBZ Recomp\ELF\SLUS_214.42"
MAP = os.path.join(os.getcwd(), "sdbz_func_map_merged.csv")
TABLE = 0x4E6C90


def load_segments(path):
    with open(path, "rb") as f:
        data = f.read()
    e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, _pa, p_filesz, _pm, p_flags = \
            struct.unpack_from("<IIIIIII", data, off)
        if p_type == 1:
            segs.append((p_vaddr, data[p_offset:p_offset + p_filesz], p_flags))
    return segs


def word_at(segs, addr):
    for base, blob, _f in segs:
        if base <= addr < base + len(blob) - 3:
            return struct.unpack_from("<I", blob, addr - base)[0]
    return None


def is_code_addr(a):
    return a is not None and 0x100000 <= a < 0x600000 and (a & 3) == 0


def main():
    segs = load_segments(ELF)

    # --- 1. full extent of the table -------------------------------------
    print("--- pointer table extent ---")
    targets = []
    a = TABLE
    while True:
        w = word_at(segs, a)
        if not is_code_addr(w):
            break
        targets.append((a, w))
        a += 4
    print(f"  {len(targets)} consecutive code pointers, "
          f"0x{TABLE:08x}-0x{a-4:08x}")
    print(f"  first: 0x{targets[0][1]:08x}   last: 0x{targets[-1][1]:08x}")
    asc = all(targets[i][1] < targets[i + 1][1] for i in range(len(targets) - 1))
    print(f"  strictly ascending: {asc}")
    lo = min(t[1] for t in targets)
    hi = max(t[1] for t in targets)
    print(f"  target span: 0x{lo:08x} - 0x{hi:08x}")
    print()

    # --- 2. what precedes each target ------------------------------------
    print("--- instruction before each target (boundary test) ---")
    for _ta, t in targets:
        prev = word_at(segs, t - 4)
        op = prev >> 26
        fn = prev & 0x3F
        if prev == 0x03E00008:
            kind = "jr $ra (RETURN)"
        elif op == 0 and fn == 0x08:
            kind = "jr (indirect jump)"
        elif op == 2:
            kind = "j (jump)"
        elif op == 4 and ((prev >> 21) & 0x1F) == 0 and ((prev >> 16) & 0x1F) == 0:
            kind = "b (uncond branch)"
        elif prev == 0:
            kind = "nop"
        else:
            kind = "FALL-THROUGH (not a boundary)"
        print(f"  0x{t:08x}  prev=0x{prev:08x}  {kind}")
    print()

    # --- 3. what the merged map thinks owns the region -------------------
    print("--- merged-map coverage of the target span ---")
    owners = []
    with open(MAP) as f:
        r = csv.reader(f)
        next(r)
        for name, s, e, _z in r:
            s, e = int(s, 16), int(e, 16)
            if e > lo and s < hi + 0x10:
                owners.append((name, s, e))
    if not owners:
        print(f"  NO function defined overlapping 0x{lo:08x}-0x{hi:08x}")
    for name, s, e in owners:
        print(f"  {name}  0x{s:08x}-0x{e:08x}")
    print()

    # nearest defined function on either side
    prev_fn = next_fn = None
    with open(MAP) as f:
        r = csv.reader(f)
        next(r)
        for name, s, e, _z in r:
            s, e = int(s, 16), int(e, 16)
            if e <= lo and (prev_fn is None or e > prev_fn[2]):
                prev_fn = (name, s, e)
            if s >= hi and (next_fn is None or s < next_fn[1]):
                next_fn = (name, s, e)
    print("--- nearest defined neighbours ---")
    if prev_fn:
        print(f"  before: {prev_fn[0]} 0x{prev_fn[1]:08x}-0x{prev_fn[2]:08x}"
              f"   gap 0x{lo - prev_fn[2]:x}")
    if next_fn:
        print(f"  after : {next_fn[0]} 0x{next_fn[1]:08x}-0x{next_fn[2]:08x}"
              f"   gap 0x{next_fn[1] - hi:x}")


main()
