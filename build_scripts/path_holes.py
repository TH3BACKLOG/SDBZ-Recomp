"""Which functions on the live PCSX2 title-screen path does our recomp not have?

Roots are the frame entry points from a real PCSX2 backtrace taken while the game
sits on the title screen. From each root we walk the *direct* call graph in the ELF
(j / jal only) to closure, then report every reachable target that has no
g_ps2RecompiledFunctionTable slot -- i.e. the holes that lie on the path the real
game takes between the memory-card screen and the title screen.
"""
import os
import sys

ROOT = r"F:\SDBZ Recomp"
sys.path.insert(0, os.path.join(ROOT, "build_scripts"))
from find_dispatch_holes import (  # noqa: E402
    DEFAULT_CSV, DEFAULT_ELF, DEFAULT_REG,
    load_ranges, load_registered, make_owner_lookup,
)
from mips_r5900_disassembler import load_elf  # noqa: E402

ROOTS = [
    0x001751C0, 0x001721E0, 0x001712D0, 0x00104C00, 0x00199840,
    0x00421EA0, 0x00422630, 0x0008FEFC, 0x00100210,
]

CODE_LO, CODE_HI = 0x00100000, 0x00500000

segments, _ = load_elf(DEFAULT_ELF)
registered = load_registered(DEFAULT_REG)
ranges = load_ranges(DEFAULT_CSV)
owner = make_owner_lookup(ranges)


def word_at(addr):
    for vaddr, data in segments:
        off = addr - vaddr
        if 0 <= off < len(data) - 3:
            return int.from_bytes(data[off:off + 4], "little")
    return None


def callees(start, end):
    """Direct j/jal targets inside [start, end)."""
    out = []
    addr = start
    while addr < end:
        w = word_at(addr)
        if w is None:
            break
        op = w >> 26
        if op in (2, 3):
            t = (addr & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if CODE_LO <= t < CODE_HI:
                out.append((t, "jal" if op == 3 else "j", addr))
        addr += 4
    return out


seen = set()
holes = {}          # target -> {"sites": [...], "kind": ...}
queue = list(ROOTS)

while queue:
    fn = queue.pop()
    if fn in seen:
        continue
    seen.add(fn)
    own = owner(fn)
    if own is None:
        continue                       # no body to walk into
    for target, kind, site in callees(own[0], own[1]):
        if target in registered:
            if target not in seen:
                queue.append(target)
            continue
        entry = holes.setdefault(
            target,
            {"sites": [], "kind": "missing-slot" if owner(target) else "missing-body"},
        )
        entry["sites"].append((site, kind))
        if target not in seen:
            queue.append(target)       # walk through the hole too, if a body covers it


print(f"reachable functions walked: {len(seen)}")
bodies = [t for t, h in holes.items() if h["kind"] == "missing-body"]
slots = [t for t, h in holes.items() if h["kind"] == "missing-slot"]
print(f"holes on this path: {len(holes)}  ({len(bodies)} missing-body, {len(slots)} missing-slot)\n")

print("MISSING-BODY on the live title-screen path (need an override):")
for t in sorted(bodies):
    sites = holes[t]["sites"]
    kinds = "/".join(sorted({k for _, k in sites}))
    srcs = ", ".join(f"0x{pc:x}" for pc, _ in sites[:4])
    print(f"  0x{t:08x}  n={len(sites):<3} {kinds:<4} from {srcs}")

print("\nMISSING-SLOT on the live title-screen path (body exists, entry not registered):")
for t in sorted(slots):
    sites = holes[t]["sites"]
    own = owner(t)
    srcs = ", ".join(f"0x{pc:x}" for pc, _ in sites[:4])
    print(f"  0x{t:08x}  n={len(sites):<3} inside {own[2]} "
          f"[0x{own[0]:08x}..0x{own[1]:08x}) from {srcs}")
