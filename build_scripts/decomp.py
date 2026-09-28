#!/usr/bin/env python3
"""Fetch IDA pseudo-C for a guest function by ADDRESS, in O(1).

WHY THIS EXISTS. The IDA decompile dumps in ida_scripts/ hold 17,010 functions
for the EE image alone (16 MB of text). On 2026-09-12 a single grep into that
file identified sub_165300 as the SofDec handle drain and sub_1652A8 as its
8-slot pump -- after six rounds of manual MIPS disassembly had produced the
same answer one basic block at a time. Pseudo-C answers "what does this do"
in one read; disassembly answers it in six.

The dumps were nonetheless going unused, because the only access pattern was
"grep 16 MB and hope the name matches". Names do not match: IDA called the
drain `wrap_sif_unk_e` and the idle test `obj_field_load_call_z_15`, and
neither has anything to do with SIF. Addresses are the only stable key
([[feedback_diff_by_address_not_filename]]), so this indexes by address.

    decomp.py get 0x165300              # one function
    decomp.py get 0x165300 --callees    # ... plus everything it calls
    decomp.py get 0x165300 -b ARKD_DVD  # a different binary's dump
    decomp.py name wrap_sif_unk_e       # name -> address
    decomp.py grep "a1 + 72"            # search bodies, report addresses
    decomp.py index --rebuild           # force a reindex

KNOWN LIMIT, and it is the same one eeref.py carries: this is IDA's STATIC
view. A function IDA failed to decompile is absent, and an absent entry means
"not in the dump", never "does not exist" ([[feedback_no_guessing]]). When the
distinction matters, fall back to mips_r5900_disassembler.py, which decodes
the ELF bytes directly and cannot be fooled by a missing analysis.

Dump headers look exactly like this, one per function:

    ; ==== wrap_sif_unk_e @ 0x165300 ====
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DUMP_DIR = os.path.join(REPO, "ida_scripts")

# ; ==== <name> @ 0x<ADDR> ====
HEADER_RE = re.compile(rb"^;\s*={2,}\s*(.+?)\s+@\s+0x([0-9A-Fa-f]+)\s*={2,}\s*$")

DEFAULT_BINARY = "SLUS_214_42"

# A callee in the pseudo-C is a bare identifier followed by "(". Drop the C
# keywords and IDA's cast/intrinsic vocabulary, or --callees chases `if` and
# `while` forever.
_NOT_A_CALL = {
    "if", "while", "for", "switch", "return", "sizeof", "do", "else",
    "int", "char", "void", "unsigned", "signed", "float", "double",
    "__int8", "__int16", "__int32", "__int64", "bool", "long", "short",
    "LOBYTE", "HIBYTE", "LOWORD", "HIWORD", "LODWORD", "HIDWORD",
    "BYTEn", "WORDn", "DWORDn", "SLOBYTE", "SHIBYTE", "SLOWORD", "SHIWORD",
    "SLODWORD", "SHIDWORD", "BYTE1", "BYTE2", "BYTE3", "WORD1", "WORD2",
    "qmemcpy", "__ROL__", "__ROR__", "__CFSHL__", "__OFSUB__", "__SPAIR64__",
}
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def dump_path(binary: str) -> str:
    return os.path.join(DUMP_DIR, f"decompiles_{binary}.txt")


def index_path(binary: str) -> str:
    return os.path.join(DUMP_DIR, f"decompiles_{binary}.index.json")


def build_index(binary: str, quiet: bool = False) -> dict:
    """Scan the dump once, recording the byte offset of every function header.

    Offsets are byte offsets into the raw file, so `get` can seek straight to a
    function instead of rescanning 16 MB. The file is read in binary and
    decoded per-function; decoding the whole dump up front would cost ~3x the
    memory and change byte offsets on any non-ASCII line.
    """
    src = dump_path(binary)
    if not os.path.exists(src):
        raise SystemExit(f"no dump for {binary!r}: {src}")

    by_addr: dict[str, list] = {}
    by_name: dict[str, str] = {}
    starts: list[tuple[int, str]] = []  # (offset, addr) in file order

    with open(src, "rb") as fh:
        offset = 0
        for raw in fh:
            m = HEADER_RE.match(raw.rstrip(b"\r\n"))
            if m:
                name = m.group(1).decode("utf-8", "replace")
                addr = int(m.group(2), 16)
                key = f"0x{addr:x}"
                by_addr[key] = [offset, name]
                # First name wins: the dump occasionally repeats a thunk.
                by_name.setdefault(name, key)
                starts.append((offset, key))
            offset += len(raw)
        total = offset

    # Each function ends where the next one begins. Recorded explicitly so
    # `get` never has to guess a terminator -- bodies legitimately contain
    # lines that look like separators.
    for i, (off, key) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else total
        by_addr[key].append(end)

    index = {
        "binary": binary,
        "source": os.path.basename(src),
        "source_size": os.path.getsize(src),
        "source_mtime": int(os.path.getmtime(src)),
        "count": len(by_addr),
        "by_addr": by_addr,      # "0xADDR" -> [start, name, end]
        "by_name": by_name,      # name -> "0xADDR"
    }

    tmp = index_path(binary) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(index, fh)
    os.replace(tmp, index_path(binary))  # [[feedback_never_open_w_on_a_real_file]]

    if not quiet:
        print(f"indexed {len(by_addr)} functions from {os.path.basename(src)}",
              file=sys.stderr)
    return index


def load_index(binary: str, rebuild: bool = False) -> dict:
    """Return the index, rebuilding it if absent or stale.

    Staleness is size+mtime against the dump. A regenerated dump with stale
    offsets would return the WRONG function body while looking perfectly
    healthy, which is the worst failure mode available here.
    """
    path = index_path(binary)
    src = dump_path(binary)
    if not rebuild and os.path.exists(path) and os.path.exists(src):
        try:
            with open(path, encoding="utf-8") as fh:
                idx = json.load(fh)
            if (idx.get("source_size") == os.path.getsize(src)
                    and idx.get("source_mtime") == int(os.path.getmtime(src))):
                return idx
            print(f"index stale for {binary}, rebuilding", file=sys.stderr)
        except (OSError, ValueError):
            print(f"index unreadable for {binary}, rebuilding", file=sys.stderr)
    return build_index(binary)


def read_function(binary: str, idx: dict, key: str) -> str:
    start, _name, end = idx["by_addr"][key]
    with open(dump_path(binary), "rb") as fh:
        fh.seek(start)
        blob = fh.read(end - start)
    return blob.decode("utf-8", "replace").rstrip()


def norm_addr(text: str) -> str:
    """Accept 0x165300, 165300, 0x165300L, sub_165300 -- all the same key."""
    t = text.strip().lower()
    m = re.search(r"(?:0x)?([0-9a-f]+)$", t)
    if not m:
        raise SystemExit(f"cannot parse address: {text!r}")
    return f"0x{int(m.group(1), 16):x}"


def resolve(idx: dict, token: str) -> str | None:
    """Token -> address key, by name first, then as a literal address."""
    if token in idx["by_name"]:
        return idx["by_name"][token]
    m = re.fullmatch(r"(?:sub_|loc_|fn_|j_)?(?:0[xX])?([0-9A-Fa-f]{5,8})", token)
    if m:
        key = f"0x{int(m.group(1), 16):x}"
        if key in idx["by_addr"]:
            return key
    return None


def cmd_get(args) -> int:
    idx = load_index(args.binary)
    key = norm_addr(args.address)
    if key not in idx["by_addr"]:
        print(f"{key}: NOT IN DUMP ({idx['count']} functions indexed for "
              f"{args.binary}).\nIDA may have failed to decompile it -- this is "
              f"'absent from the dump', NOT 'does not exist'.\nFall back to: "
              f"python build_scripts/mips_r5900_disassembler.py <ELF> {key} 40",
              file=sys.stderr)
        return 1

    seen = {key}
    queue = [(key, 0)]
    out = []
    while queue:
        cur, depth = queue.pop(0)
        out.append(read_function(args.binary, idx, cur))
        if depth >= args.callees:
            continue
        body = out[-1]
        for token in dict.fromkeys(CALL_RE.findall(body)):
            if token in _NOT_A_CALL:
                continue
            target = resolve(idx, token)
            if target and target not in seen:
                seen.add(target)
                queue.append((target, depth + 1))

    print(("\n\n" + "-" * 70 + "\n\n").join(out))
    if args.callees and len(seen) > 1:
        print(f"\n[{len(seen)} functions, depth {args.callees}]", file=sys.stderr)
    return 0


def cmd_name(args) -> int:
    idx = load_index(args.binary)
    hits = [(a, v[1]) for a, v in idx["by_addr"].items()
            if args.pattern.lower() in v[1].lower()]
    if not hits:
        print(f"no name matching {args.pattern!r}", file=sys.stderr)
        return 1
    for addr, name in sorted(hits, key=lambda h: int(h[0], 16))[:args.limit]:
        print(f"{addr:>10}  {name}")
    if len(hits) > args.limit:
        print(f"... {len(hits) - args.limit} more", file=sys.stderr)
    return 0


def cmd_grep(args) -> int:
    """Search function BODIES, report the owning function's address.

    This is the query eeref.py cannot answer: "who dereferences +72 and +68
    together", "who touches this magic constant". Reports the function, not
    the line, because the function is what you then read.
    """
    idx = load_index(args.binary)
    rx = re.compile(args.pattern, 0 if args.case else re.IGNORECASE)
    order = sorted(idx["by_addr"].items(), key=lambda kv: kv[1][0])

    hits = 0
    with open(dump_path(args.binary), "rb") as fh:
        for key, (start, name, end) in order:
            fh.seek(start)
            body = fh.read(end - start).decode("utf-8", "replace")
            found = rx.findall(body)
            if found:
                hits += 1
                print(f"{key:>10}  {name}  ({len(found)} hit(s))")
                if hits >= args.limit:
                    print(f"... stopped at {args.limit}", file=sys.stderr)
                    break
    if not hits:
        print(f"no body matching {args.pattern!r}", file=sys.stderr)
        return 1
    return 0


def cmd_index(args) -> int:
    idx = build_index(args.binary) if args.rebuild else load_index(args.binary)
    print(f"{idx['binary']}: {idx['count']} functions  <- {idx['source']}")
    return 0


def cmd_list(_args) -> int:
    if not os.path.isdir(DUMP_DIR):
        raise SystemExit(f"no dump directory: {DUMP_DIR}")
    for fn in sorted(os.listdir(DUMP_DIR)):
        if fn.startswith("decompiles_") and fn.endswith(".txt"):
            binary = fn[len("decompiles_"):-len(".txt")]
            size = os.path.getsize(os.path.join(DUMP_DIR, fn))
            indexed = "indexed" if os.path.exists(index_path(binary)) else "-"
            print(f"{binary:<24} {size / 1048576:>7.1f} MB  {indexed}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Fetch IDA pseudo-C by guest address.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("-b", "--binary", default=DEFAULT_BINARY,
                    help=f"dump to use (default: {DEFAULT_BINARY}); "
                         f"see `decomp.py list`")
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("get", help="print one function's pseudo-C")
    g.add_argument("address")
    g.add_argument("--callees", nargs="?", type=int, const=1, default=0,
                   metavar="DEPTH",
                   help="also print called functions, to DEPTH (default 1)")
    g.set_defaults(func=cmd_get)

    n = sub.add_parser("name", help="find addresses by name substring")
    n.add_argument("pattern")
    n.add_argument("--limit", type=int, default=40)
    n.set_defaults(func=cmd_name)

    r = sub.add_parser("grep", help="search bodies, report owning functions")
    r.add_argument("pattern")
    r.add_argument("--limit", type=int, default=40)
    r.add_argument("--case", action="store_true", help="case-sensitive")
    r.set_defaults(func=cmd_grep)

    i = sub.add_parser("index", help="build or report the index")
    i.add_argument("--rebuild", action="store_true")
    i.set_defaults(func=cmd_index)

    sub.add_parser("list", help="list available dumps").set_defaults(func=cmd_list)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
