"""Classify the addresses the regenerated dispatch table LOST relative to live.

"Reachable" (lost_addr_reach.py) only says something materializes the address.
It does NOT say the regeneration mishandles it. Three outcomes are possible,
and this separates them mechanically:

  BENIGN-ALIAS : the address is interior to a function the NEW table DOES have,
                 and that function's generated .cpp carries an entry guard for
                 it (`ctx->pc == 0xADDR`). Dispatch still works -- the address
                 simply moved from its own slot into a neighbour's body.

  NOT-CODE     : the address does not live in an executable segment. The old
                 run classified a data word as a function entry; the new run
                 correctly does not. Dropping it is a fix, not a regression.

  REGRESSION   : in an exec segment, no owning function, or an owning function
                 with no entry guard. These are the real risk.

Read-only. Touches nothing outside the scratch dir and the ELF.
"""
import bisect
import os
import re
import struct

SCRATCH = os.path.dirname(os.path.abspath(__file__))
ELF = r"F:\SDBZ Recomp\ELF\SLUS_214.42"
LOST = os.path.join(SCRATCH, "lost.txt")
NEWTAB = os.path.join(SCRATCH, "regen_probe", "register_functions.cpp")
SRCDIR = os.path.join(SCRATCH, "regen_probe")


def load_segments(path):
    with open(path, "rb") as f:
        data = f.read()
    e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C)[0]
    execs, alls = [], []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, _pa, p_filesz, _pm, p_flags = \
            struct.unpack_from("<IIIIIII", data, off)
        if p_type == 1:
            seg = (p_vaddr, data[p_offset:p_offset + p_filesz])
            alls.append(seg)
            if p_flags & 0x1:
                execs.append(seg)
    return execs, alls


def reachable(execs, alls, want):
    """Same materialization test as reachability_scan.py."""
    refs = {a: [] for a in want}
    for base, blob in execs:
        n = len(blob) // 4
        words = struct.unpack_from(f"<{n}I", blob, 0)
        lui_hi = {}
        for i in range(n):
            w = words[i]
            op = w >> 26
            if op == 0x0F:
                lui_hi[(w >> 16) & 0x1F] = ((w & 0xFFFF) << 16, base + i * 4)
            elif op in (0x09, 0x0D):
                rs = (w >> 21) & 0x1F
                rt = (w >> 16) & 0x1F
                if rs in lui_hi:
                    hi, la = lui_hi[rs]
                    imm = w & 0xFFFF
                    if op == 0x09 and imm & 0x8000:
                        val = (hi - (0x10000 - imm)) & 0xFFFFFFFF
                    else:
                        val = (hi + imm) & 0xFFFFFFFF
                    if val in refs:
                        refs[val].append(f"code @ 0x{la:08x}")
                    if rt != rs:
                        lui_hi.pop(rs, None)
    for base, blob in alls:
        n = len(blob) // 4
        words = struct.unpack_from(f"<{n}I", blob, 0)
        for i in range(n):
            if words[i] in refs:
                refs[words[i]].append(f"data @ 0x{base + i*4:08x}")
    return refs


def load_new_table(path):
    """addr -> symbol, for every non-null slot in the regenerated table."""
    pat = re.compile(r"=\s*(\w+);\s*//\s*(0x[0-9a-fA-F]+)\s*$")
    out = {}
    with open(path, "r", errors="ignore") as f:
        for line in f:
            m = pat.search(line.rstrip())
            if m:
                out[int(m.group(2), 16)] = m.group(1)
    return out


def in_exec(execs, addr):
    return any(b <= addr < b + len(d) for b, d in execs)


def has_entry_guard(sym, addr):
    """Does the owning function's generated .cpp accept this PC as an ENTRY?

    Must be an equality test (`ctx->pc == 0xADDR`) or a switch `case 0xADDR:`.
    A bare `0xADDR` substring is NOT sufficient: generated bodies are full of
    `ctx->pc = 0xADDR;` ASSIGNMENTS for PC tracking plus `// 0xaddr: ...`
    disassembly comments, both of which match every address in the function's
    range and would mark every alias benign. That false all-clear is exactly
    what the first version of this script produced.
    """
    path = os.path.join(SRCDIR, sym + ".cpp")
    if not os.path.exists(path):
        return None
    with open(path, "r", errors="ignore") as f:
        body = f.read()
    pat = re.compile(
        r"(?:==\s*0x0*%X[uU]?\b|case\s+0x0*%X[uU]?\s*:)" % (addr, addr),
        re.IGNORECASE)
    return bool(pat.search(body))


def main():
    with open(LOST) as f:
        lost = sorted({int(l.strip(), 16) for l in f if l.strip()})

    execs, alls = load_segments(ELF)
    refs = reachable(execs, alls, set(lost))
    risky = sorted(a for a in lost if refs[a])

    newtab = load_new_table(NEWTAB)
    keys = sorted(newtab)

    buckets = {"BENIGN-ALIAS": [], "NOT-CODE": [], "REGRESSION": [], "NO-OWNER": []}
    detail = {}

    for a in risky:
        if not in_exec(execs, a):
            buckets["NOT-CODE"].append(a)
            continue
        i = bisect.bisect_right(keys, a) - 1
        if i < 0:
            buckets["NO-OWNER"].append(a)
            continue
        owner = keys[i]
        sym = newtab[owner]
        guard = has_entry_guard(sym, a)
        detail[a] = (owner, sym, guard)
        if guard:
            buckets["BENIGN-ALIAS"].append(a)
        else:
            buckets["REGRESSION"].append(a)

    print(f"lost total          : {len(lost)}")
    print(f"  reachable         : {len(risky)}")
    print()
    for k in ("BENIGN-ALIAS", "NOT-CODE", "REGRESSION", "NO-OWNER"):
        print(f"  {k:<14}: {len(buckets[k])}")
    print()
    print("--- REGRESSION sample (real risk) ---")
    for a in buckets["REGRESSION"][:25]:
        owner, sym, guard = detail[a]
        delta = a - owner
        print(f"0x{a:08x}  owner 0x{owner:08x} (+0x{delta:x})  {sym}"
              f"{'  [no .cpp]' if guard is None else ''}")
        print(f"            ref: {refs[a][0]}")
    if len(buckets["REGRESSION"]) > 25:
        print(f"... +{len(buckets['REGRESSION'])-25} more")

    out = os.path.join(SCRATCH, "regression_addrs.txt")
    with open(out, "w") as f:
        for a in buckets["REGRESSION"]:
            f.write(f"{a:08x}\n")
    print(f"\nwrote {out}")


main()
