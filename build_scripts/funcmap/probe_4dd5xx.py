"""Measure the extent of the four functions IDA missed at 0x4dd5xx.

These are referenced as data words from a function-pointer table at
0x4e6c90-0x4e6c9c. IDA defined NO function anywhere in the region, so the
merged map inherits the gap and the recompiler emits no dispatch slot.

Do NOT assume each function ends where the next begins -- measure it. A MIPS
function ends at `jr $ra` (0x03e00008) plus its delay slot. Print the raw
disassembly around each entry so the boundary is visible, not inferred.

Read-only: reads the ELF, prints. Writes nothing.
"""
import struct

ELF = r"F:\SDBZ Recomp\ELF\SLUS_214.42"
ENTRIES = [0x4DD500, 0x4DD570, 0x4DD5B0, 0x4DD5E0]
JR_RA = 0x03E00008


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
    for base, blob, flags in segs:
        if base <= addr < base + len(blob) - 3:
            return struct.unpack_from("<I", blob, addr - base)[0], flags
    return None, 0


def main():
    segs = load_segments(ELF)

    # Confirm the pointer table really holds these values.
    print("--- pointer table at 0x4e6c90 ---")
    for a in range(0x4E6C88, 0x4E6CA8, 4):
        w, _ = word_at(segs, a)
        mark = "  <-- entry" if w in ENTRIES else ""
        print(f"  0x{a:08x}: 0x{w:08x}{mark}")
    print()

    for start in ENTRIES:
        w, flags = word_at(segs, start)
        execbit = "exec" if flags & 1 else "NOT-EXEC"
        print(f"--- 0x{start:08x} ({execbit}) ---")
        end = None
        a = start
        while a < start + 0x400:
            w, _ = word_at(segs, a)
            if w is None:
                break
            if w == JR_RA:
                end = a + 8          # jr $ra + delay slot
                break
            a += 4
        if end is None:
            print("  no `jr $ra` within 0x400 -- NOT resolved, do not add")
            print()
            continue
        print(f"  jr $ra at 0x{end-8:08x}  ->  end 0x{end:08x}  size 0x{end-start:x}")
        # show first and last two instructions as evidence
        for label, addr in (("first", start), ("first+4", start + 4),
                            ("jr", end - 8), ("delay", end - 4)):
            w, _ = word_at(segs, addr)
            print(f"    {label:<8} 0x{addr:08x}: 0x{w:08x}")
        nxt = [e for e in ENTRIES if e > start]
        if nxt and end > nxt[0]:
            print(f"  !! overlaps next entry 0x{nxt[0]:08x} -- OVERLAP, do not add")
        print()


main()
