#!/usr/bin/env python3
"""Extract a BUG-008 analyzer-folded tail-call block from a sibling runner/*.cpp
file into a standalone override function ready to paste into game_overrides.cpp.

Usage:
    python extract_folded_function.py --file <path-to-sibling-runner-cpp> --label <hex-addr>

The sibling file must contain a `label_<addr>:` inside its enclosing function body;
the script slices from that label to the enclosing function's closing brace,
strips the label line itself, and wraps the remainder in a new function named
fn_<ADDR>_0x<addr>.
"""
import argparse
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True, help="Path to sibling runner/*.cpp containing the label")
    ap.add_argument("--label", required=True, help="Target hex address (e.g. 151830 or 0x151830)")
    ap.add_argument("--out", help="Optional output file (default: stdout)")
    args = ap.parse_args()

    addr = args.label.lower().replace("0x", "")
    addr_int = int(addr, 16)
    label_name = f"label_{addr}"

    with open(args.file, "r", encoding="utf-8") as f:
        lines = f.readlines()

    label_line_idx = None
    for i, line in enumerate(lines):
        if re.match(rf"^{re.escape(label_name)}:\s*$", line.strip()):
            label_line_idx = i
            break

    if label_line_idx is None:
        print(f"ERROR: could not find '{label_name}:' in {args.file}", file=sys.stderr)
        sys.exit(1)

    # Find the enclosing function's closing brace: walk forward from the label
    # to the final '}' at column 0 (the outer function's closing brace).
    end_idx = None
    for i in range(label_line_idx, len(lines)):
        if lines[i].rstrip("\n") == "}":
            end_idx = i
            break

    if end_idx is None:
        print("ERROR: could not find enclosing function's closing brace", file=sys.stderr)
        sys.exit(1)

    # Body is everything strictly between the label line and the closing brace.
    body_lines = lines[label_line_idx + 1:end_idx]
    body = "".join(body_lines)

    # Brace-balance sanity check.
    open_count = body.count("{")
    close_count = body.count("}")
    if open_count != close_count:
        print(
            f"ERROR: brace mismatch in extracted body ({{={open_count}, }}={close_count}); "
            "boundary guess is likely wrong, refusing to emit",
            file=sys.stderr,
        )
        sys.exit(1)

    addr_upper = addr.upper()
    fn_name = f"fn_{addr_upper}_0x{addr}"

    out = []
    out.append(f"void {fn_name}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {{\n")
    out.append("#ifdef PS2_FUNCTION_LOG_TRACKER\n")
    out.append(f'    PS_LOG_ENTRY("{fn_name}");\n')
    out.append("#endif\n")
    out.append(body)
    out.append("}\n")
    out.append("\n")
    out.append(f"// runtime.registerFunction(0x{addr}u, &{fn_name});\n")

    text = "".join(out)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"Wrote {args.out}", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
