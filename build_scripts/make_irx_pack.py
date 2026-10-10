#!/usr/bin/env python3
"""Build a self-contained IRX-labeling prompt pack for an external model
(GitHub Copilot / Fable), and verify the CSV it hands back.

The pack is deliberately standalone: it inlines the decompiled source and the
ps2sdk export table, so the answering model needs no repo access and no IDA.

  build:  python make_irx_pack.py build PADMAN [--chunk-size 60]
  verify: python make_irx_pack.py verify PADMAN --csv <reply.csv>

`verify` is the adjudication gate -- it rejects invented addresses, missing
rows, bad naming convention and duplicate labels before anything is applied.
Replaces the vanished build_scripts/irx_label.py for the read-only half of the
workflow; the apply half still needs an idalib tool.
"""
import argparse
import csv
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMPS = os.path.join(ROOT, "ida_scripts")
SDKJSON = os.path.join(DUMPS, "idapy_ps2", "IOP")
OUTDIR = os.path.join(ROOT, "build_scripts", "copilot_packs")

FN_RE = re.compile(r"^; ==== (\S+) @ (0x[0-9A-Fa-f]+) ====$")
UNNAMED = ("sub_", "unk_", "loc_", "j_sub_", "nullsub_")
NAME_RE = re.compile(r"^irx_[a-z0-9]+_[A-Za-z0-9_]+$")


def parse_dump(path):
    """-> [(name, addr_int, body_text)] in file order."""
    fns, cur, buf = [], None, []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = FN_RE.match(line.rstrip("\n"))
            if m:
                if cur:
                    fns.append((cur[0], cur[1], "".join(buf).strip()))
                cur, buf = (m.group(1), int(m.group(2), 16)), []
            elif cur:
                buf.append(line)
    if cur:
        fns.append((cur[0], cur[1], "".join(buf).strip()))
    return fns


def sdk_exports(module):
    p = os.path.join(SDKJSON, module.lower() + ".json")
    if not os.path.exists(p):
        return None
    with open(p, "r", encoding="utf-8") as fh:
        return json.load(fh)


HEADER = """# IRX function-labeling task -- {module}.IRX (PlayStation 2 IOP module)

You are reverse-engineering a PlayStation 2 **IOP** module (MIPS R3000A, little-endian).
`{module}.IRX` is {blurb}
The decompiled C below came from IDA Pro. Function names of the form `sub_XXXX` are
**unlabeled** -- your job is to propose a semantic name for each one.

## Output contract -- read this before writing anything

Reply with **CSV only**, no prose, no markdown fence, one row per function I gave you,
in the same order, with this exact header line:

```
addr,old_name,proposed_name,confidence,evidence
```

- `addr` -- copy verbatim from the input (`0x...`). Never invent an address.
- `proposed_name` -- `irx_{modlower}_SemanticName` (PascalCase after the prefix), **or**
  the literal `UNKNOWN` if you cannot justify a name from the body.
- `confidence` -- `high` / `medium` / `low`.
- `evidence` -- ONE short clause naming what in the body justifies the name
  (a called export, a register/port constant, a struct offset, a control-flow shape).
  Use a semicolon instead of a comma inside this field, or quote the field.
  If `proposed_name` is `UNKNOWN`, write `UNKNOWN` here too.

## Rules that matter more than coverage

1. **`UNKNOWN` is a correct answer.** A wrong-but-plausible label is worse than a blank:
   it gets applied to a database and read as fact for months. Guessing is the failure mode
   this task is designed to avoid.
2. Name what the code **does**, not what the module is about. `irx_{modlower}_Init` is only
   correct if that function actually initialises something.
3. Do not name a function from its callers' names alone -- you cannot see all callers.
4. If two functions have near-identical bodies, say so in `evidence` rather than inventing
   a distinction.
5. `confidence: high` is reserved for a body that is decisive on its own (a known SDK call
   sequence, a hardware register write, an unmistakable dispatcher).

## Ground truth you may rely on

These are the module's **real exported entry points**, from the ps2sdk export table for
`{module}` (ordinal -> name). If a `sub_` body matches one of these semantics, prefer the
SDK name in PascalCase after the prefix.

```json
{exports}
```

{extra}
## Functions ({count} in this part{partof})

"""

BLURBS = {
    "PADMAN": "the controller (DualShock) driver. It talks to the pad through SIO2 and\nexposes a `pad*` library API to the EE over SIF RPC.",
    "SIO2MAN": "the SIO2 serial-bus manager -- the transport layer under PADMAN and MCMAN.",
    "MCSERV": "the memory-card service layer sitting on top of MCMAN.",
    "MCMAN": "the memory-card low-level manager (block/page IO, FAT, directory).",
    "LIBSD": "the SPU2 sound driver.",
    "CRI_ADXI": "the CRI ADX audio decoder used by the SofDec movie player.",
    "ARKD_DVD": "the game-specific DVD/CD reader module.",
}

EXTRA = {
    "PADMAN": """### Useful anchors for this module

- SIO2 hardware registers live at `0xBF808200`+ on the IOP; SIO2 DMA at `0xBF801500`+.
- Pad state machine constants: 0 = disconnected, 1 = findpad, 2 = findctp1,
  4 = execcmd, 5 = stable, 6 = error.
- Pad report frames are 32 bytes; button data is 2 bytes big-endian at offset 2,
  analog sticks at offsets 4-7, pressure at 8-19.
- Terminal ids: 0x41 = digital, 0x73 = analog/dualshock, 0x79 = dualshock2.
""",
    "SIO2MAN": """### Useful anchors for this module

- SIO2 registers: `0xBF808200` (send buf), `0xBF808230` (recv/FIFO), `0xBF808268` (ctrl),
  `0xBF80826C` (recv2), `0xBF808280` (intr).
- A transfer queue entry holds TR_CTRL words, an in-buffer and an out-buffer.
""",
}


def build(module, chunk_size):
    dump = os.path.join(DUMPS, "decompiles_%s_IRX.txt" % module)
    if not os.path.exists(dump):
        sys.exit("no dump: %s" % dump)
    allfns = parse_dump(dump)
    fns = [f for f in allfns if f[0].startswith(UNNAMED)]
    if not fns:
        sys.exit("nothing unlabeled in %s" % dump)
    exports = sdk_exports(module)
    exp_txt = json.dumps(exports, indent=2) if exports else \
        "(no ps2sdk export table bundled for this module -- rely on the bodies alone)"

    parts = [fns[i:i + chunk_size] for i in range(0, len(fns), chunk_size)]
    os.makedirs(OUTDIR, exist_ok=True)
    written = []
    for idx, part in enumerate(parts, 1):
        partof = "" if len(parts) == 1 else ", part %d of %d" % (idx, len(parts))
        text = HEADER.format(
            module=module, modlower=module.lower().replace("_", ""),
            blurb=BLURBS.get(module, "an IOP driver module."),
            exports=exp_txt, extra=EXTRA.get(module, ""),
            count=len(part), partof=partof)
        chunks = [text]
        for name, addr, body in part:
            chunks.append("### %s @ 0x%X\n\n```c\n%s\n```\n\n" % (name, addr, body))
        chunks.append("\n---\n\nNow output the CSV for the %d functions above. CSV only.\n"
                      % len(part))
        out = os.path.join(OUTDIR, "pack_%s_%02d.md" % (module, idx))
        with open(out, "w", encoding="utf-8") as fh:
            fh.write("".join(chunks))
        written.append((out, len(part), os.path.getsize(out)))

    manifest = os.path.join(OUTDIR, "manifest_%s.csv" % module)
    with open(manifest, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["addr", "old_name"])
        for name, addr, _ in fns:
            w.writerow(["0x%X" % addr, name])

    print("module %s: %d unlabeled of %d total" % (module, len(fns), len(allfns)))
    for out, n, sz in written:
        print("  %s  (%d fns, %.1f KB)" % (out, n, sz / 1024.0))
    print("  %s  (adjudication manifest)" % manifest)


def verify(module, csv_path):
    manifest = os.path.join(OUTDIR, "manifest_%s.csv" % module)
    if not os.path.exists(manifest):
        sys.exit("build the pack first (missing %s)" % manifest)
    with open(manifest, newline="", encoding="utf-8") as fh:
        known = {r["addr"].lower(): r["old_name"] for r in csv.DictReader(fh)}

    rows, problems, seen, names = [], [], set(), {}
    with open(csv_path, newline="", encoding="utf-8") as fh:
        for i, r in enumerate(csv.DictReader(fh), 2):
            addr = (r.get("addr") or "").strip().lower()
            prop = (r.get("proposed_name") or "").strip()
            conf = (r.get("confidence") or "").strip().lower()
            if addr not in known:
                problems.append("line %d: INVENTED address %r" % (i, r.get("addr")))
                continue
            if addr in seen:
                problems.append("line %d: duplicate address %s" % (i, addr))
            seen.add(addr)
            if prop.upper() == "UNKNOWN" or not prop:
                continue
            if not NAME_RE.match(prop):
                problems.append("line %d: %s bad name %r (want irx_<mod>_Name)" % (i, addr, prop))
            if conf not in ("high", "medium", "low"):
                problems.append("line %d: %s bad confidence %r" % (i, addr, conf))
            if not (r.get("evidence") or "").strip():
                problems.append("line %d: %s empty evidence" % (i, addr))
            if prop in names:
                problems.append("line %d: %s collides with %s on %r" % (i, addr, names[prop], prop))
            names[prop] = addr
            rows.append((addr, known[addr], prop, conf, (r.get("evidence") or "").strip()))

    missing = sorted(set(known) - seen)
    print("manifest %d | replied %d | named %d | UNKNOWN/blank %d | missing %d"
          % (len(known), len(seen), len(rows), len(seen) - len(rows), len(missing)))
    if missing:
        print("MISSING (model dropped these): " + ", ".join(missing[:20])
              + (" ..." if len(missing) > 20 else ""))
    for p in problems:
        print("PROBLEM: " + p)
    if not problems and not missing:
        print("CLEAN -- every row accounted for. Still needs a review pass before apply.")
    return 1 if (problems or missing) else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build")
    b.add_argument("module")
    b.add_argument("--chunk-size", type=int, default=60)
    v = sub.add_parser("verify")
    v.add_argument("module")
    v.add_argument("--csv", required=True)
    a = ap.parse_args()
    if a.cmd == "build":
        build(a.module.upper(), a.chunk_size)
    elif a.cmd == "verify":
        sys.exit(verify(a.module.upper(), a.csv))
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
