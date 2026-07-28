#!/usr/bin/env python3
"""Find generated runner functions whose emitted body is cut short.

Every generated runner/*.cpp carries a header comment:

    // Address: 0x104bf0 - 0x104bf4

That range is what the recompiler actually emitted. This compares it against
the REAL function extent read from the ELF (start -> the `jr $ra` that ends it).
When the emitted end lands before the real end, the C++ body falls off its own
end without updating ctx->pc, and the runtime dispatch loop re-dispatches the
same pc forever -- an infinite loop that looks like a guest spin but is a
codegen defect. (Confirmed live case: 0x104bf0, emitted 1 of 4 instructions.)

    python find_truncated_functions.py <elf> <runner_dir> [--json out.json]

Only the first ~40 lines of each runner file are read, and nothing but the
summary is printed, so this is safe to run despite runner/ holding 30,000+
files. Worst (most instructions lost) first.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_r5900_disassembler import load_elf, read_word  # noqa: E402

HEADER_RE = re.compile(r"^//\s*Address:\s*0x([0-9a-fA-F]+)\s*-\s*0x([0-9a-fA-F]+)")
MAX_SCAN = 8192


def function_end(segs, addr):
    """(end_addr, insn_count) past the `jr $ra` delay slot, or None if not code."""
    pc = addr
    for i in range(MAX_SCAN):
        word = read_word(segs, pc)
        if word is None:
            return None
        if (word >> 26) == 0 and (word & 0x3F) == 0x08 and ((word >> 21) & 31) == 31:
            return pc + 8, i + 2
        pc += 4
    return None


def scan_header(path):
    try:
        with open(path, "r", errors="replace") as fh:
            for _ in range(40):
                line = fh.readline()
                if not line:
                    break
                m = HEADER_RE.match(line.strip())
                if m:
                    return int(m.group(1), 16), int(m.group(2), 16)
    except OSError:
        pass
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    elf_path, runner_dir = sys.argv[1], sys.argv[2]
    json_out = None
    if "--json" in sys.argv:
        json_out = sys.argv[sys.argv.index("--json") + 1]

    segs, _ = load_elf(elf_path)

    findings = []
    scanned = 0
    noheader = 0
    for name in os.listdir(runner_dir):
        if not name.endswith(".cpp"):
            continue
        hdr = scan_header(os.path.join(runner_dir, name))
        if hdr is None:
            noheader += 1
            continue
        scanned += 1
        start, emitted_end = hdr
        real = function_end(segs, start)
        if real is None:
            continue
        real_end, total = real
        # A body is only broken if it FALLS OFF its end. If the last emitted
        # instruction is an unconditional transfer (j / jr, incl. a tail call),
        # ctx->pc is set on the way out and the short range is legitimate --
        # such functions have no `jr $ra` of their own, so real_end above ran
        # on into the NEXT function and must not be trusted.
        last = read_word(segs, emitted_end - 4) if emitted_end >= start + 4 else None
        if last is not None:
            op = last >> 26
            is_j = op == 0x02
            is_jr = (op == 0 and (last & 0x3F) in (0x08, 0x09))
            if is_j or is_jr:
                continue
            # `j`/`jr` in the delay-slot position (emitted range includes the
            # slot) -- check one instruction earlier too.
            prev = read_word(segs, emitted_end - 8) if emitted_end >= start + 8 else None
            if prev is not None:
                pop = prev >> 26
                if pop == 0x02 or (pop == 0 and (prev & 0x3F) in (0x08, 0x09)):
                    continue

        if emitted_end < real_end:
            emitted = max((emitted_end - start) // 4, 0)
            findings.append({
                "lost": total - emitted,
                "addr": start,
                "emitted_end": emitted_end,
                "real_end": real_end,
                "emitted": emitted,
                "total": total,
                "file": name,
            })

    findings.sort(key=lambda f: -f["lost"])
    print(f"# scanned {scanned} runner files ({noheader} without an Address header)")
    print(f"# truncated: {len(findings)}")
    print("# lost  addr        emitted_end  real_end     emitted/total  file")
    for f in findings:
        print(f"{f['lost']:6d}  0x{f['addr']:08x}  0x{f['emitted_end']:08x}   "
              f"0x{f['real_end']:08x}   {f['emitted']}/{f['total']:<6}  {f['file']}")

    if json_out:
        with open(json_out, "w") as fh:
            json.dump(findings, fh, indent=1)
        print(f"# wrote {json_out}")


if __name__ == "__main__":
    main()
