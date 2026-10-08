#!/usr/bin/env python3
"""conformance_diff.py -- Stage B: compare OUR test outputs with PCSX2's -> report.md.

Per output record (one test word x one input vector) every stored register is compared:
  exact          -> same bits
  ULP            -> both finite floats, at most 2 units in the last place apart (rounding mode:
                    the PS2 rounds toward zero, the host to nearest; fused madd/msub can reach 2)
  FLAG           -> only a flag register differs (FCR31, VU STATUS/MAC/CLIP)
  FAIL           -> anything else (wrong value, wrong register written, NaN/Inf where PCSX2 clamps, ...)
A class is as bad as its worst record. Records PCSX2 never reached (stuck run) are SKIPPED.

    python conformance_diff.py            (reads out/manifest.json, out/ours.bin, out/pcsx2.bin)
"""
import argparse
import collections
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import conf_isa as I  # noqa: E402

RANK = {"PASS": 0, "ULP": 1, "FLAG": 2, "FAIL": 3, "CRASH": 4}
FLAG_FIELDS = {"FCR31", "STATUS", "MAC", "CLIP"}


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def f32(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


def fmt(v):
    e = (v >> 23) & 0xFF
    if e == 0xFF:
        return f"0x{v:08x} (exp255)"
    return f"0x{v:08x} ({f32(v):.7g})"


def ulp_close(a, b):
    if (a >> 23) & 0xFF == 0xFF or (b >> 23) & 0xFF == 0xFF:
        return False
    if (a ^ b) & 0x80000000:
        return (a & 0x7FFFFFFF) == 0 and (b & 0x7FFFFFFF) == 0
    return abs((a & 0x7FFFFFFF) - (b & 0x7FFFFFFF)) <= 2


def fields(groups, m):
    """-> [(name, offset in record, mask, is_float)] for the stored groups."""
    out, off = [], m["rec_hdr"]
    gs = m["group_size"]
    for g in "GFV":
        if g not in groups:
            continue
        if g == "G":
            for r in range(1, 32):
                if r in (I.K0, I.K1):
                    continue
                for lane in range(4):
                    out.append((f"{I.GPRN[r]}.w{lane}", off + 16 * r + 4 * lane, 0xFFFFFFFF, False))
            for nm, o in (("HI", 512), ("LO", 528)):
                for lane in range(4):
                    out.append((f"{nm}.w{lane}", off + o + 4 * lane, 0xFFFFFFFF, False))
            out.append(("SA", off + 544, 0xFFFFFFFF, False))
        elif g == "F":
            out += [(f"$f{r}", off + 4 * r, 0xFFFFFFFF, True) for r in range(32)]
            out += [("FCR31", off + 128, 0xFFFFFFFF, False), ("FPU ACC", off + 132, 0xFFFFFFFF, True)]
        else:
            for r in range(32):
                for lane in range(4):
                    out.append((f"$vf{r}.{'xyzw'[lane]}", off + 16 * r + 4 * lane, 0xFFFFFFFF, True))
            for n in I.VI_STORE:
                nm = f"$vi{n}" if n < 16 else I.CTRL_NAMES[n]
                # R holds 23 random bits; PCSX2 reads back only those (ours keeps the 1.0 exponent too)
                mask = 0xFFFF if n < 16 else (0x7FFFFF if n == 20 else 0xFFFFFFFF)
                out.append((nm, off + 512 + 4 * n, mask, n in (21, 22)))
            for lane in range(4):
                out.append((f"VU ACC.{'xyzw'[lane]}", off + 640 + 4 * lane, 0xFFFFFFFF, True))
        off += gs[g]
    return out


def operand_inputs(t, img, m):
    """The game word's source operands, read from the input image (for the report)."""
    w = int(t["word"], 16)
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    gi = m["group_in_off"]
    g = t["groups"]
    if g == "F":
        return f"fs=$f{rd}={fmt(u32(img, gi['F'] + 4 * rd))}, ft=$f{rt}={fmt(u32(img, gi['F'] + 4 * rt))}"
    if g == "V":
        def vf(r):
            if r == 0:
                return "(0, 0, 0, 1)"
            return "(" + ", ".join(f"{f32(u32(img, gi['V'] + 16 * r + 4 * k)):.6g}" for k in range(4)) + ")"
        return f"fs=$vf{rd}={vf(rd)}, ft=$vf{rt}={vf(rt)}"

    def gpr(r):
        lo = u32(img, gi["G"] + 16 * r) | (u32(img, gi["G"] + 16 * r + 4) << 32)
        return f"{I.GPRN[r]}=0x{lo:016x}"
    return f"{gpr(rs)}, {gpr(rt)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, default=HERE / "out")
    ap.add_argument("--ours", type=Path)
    ap.add_argument("--pcsx2", type=Path)
    ap.add_argument("--report", type=Path)
    a = ap.parse_args()
    m = json.loads((a.dir / "manifest.json").read_text())
    ours = (a.ours or a.dir / "ours.bin").read_bytes()
    ref = (a.pcsx2 or a.dir / "pcsx2.bin").read_bytes()
    elf = (a.dir / "conformance.elf").read_bytes()
    pj = a.dir / "pcsx2.json"
    pinfo = json.loads(pj.read_text()) if pj.exists() else {}
    hdr = m["hdr"]
    # input images straight from the ELF's data segment (file offset = data_off + (addr - HDR))
    phoff = u32(elf, 0x1C)
    data_off = u32(elf, phoff + 32 + 4)

    def image(v):
        o = data_off + (m["in_base"] - hdr) + m["img_size"] * v
        return elf[o:o + m["img_size"]]

    base = m["out_base"]
    tags = m["vectors"]
    per_class = collections.OrderedDict()
    corrupt = 0
    for t in m["tests"]:
        flds = fields(t["groups"], m)
        res = {"verdict": "PASS", "clean": "PASS", "bad": collections.Counter(), "tags": collections.Counter(), "example": None,
               "fields": collections.Counter(), "skipped": 0}
        for v in range(len(tags)):
            ro = t["out"] - base + v * t["rec_size"]
            o_rec, r_rec = ours[ro:ro + t["rec_size"]], ref[ro:ro + t["rec_size"]]
            exp_in, exp_out = m["in_base"] + m["img_size"] * v, t["out"] + v * t["rec_size"]
            if u32(r_rec, 4) != exp_in or u32(r_rec, 8) != exp_out:
                if u32(r_rec, 4) == 0 and u32(r_rec, 8) == 0:
                    res["skipped"] += 1          # PCSX2 never ran it
                else:
                    corrupt += 1                 # k0/k1 changed under the test: an interrupt got in
                continue
            if u32(o_rec, 12) == 0xC0DEDEAD:
                verdict, diffs = "CRASH", []
            else:
                diffs = []
                for nm, off, mask, isf in flds:
                    x, y = u32(o_rec, off) & mask, u32(r_rec, off) & mask
                    if x != y:
                        kind = "FLAG" if nm in FLAG_FIELDS else ("ULP" if isf and ulp_close(x, y) else "FAIL")
                        diffs.append((RANK[kind], kind, nm, x, y, isf))
                verdict = max((d[1] for d in diffs), key=RANK.get, default="PASS")
            if verdict != "PASS":
                res["bad"][verdict] += 1
                res["tags"][tags[v]] += 1
                for d in diffs:
                    res["fields"][d[2]] += 1
            if RANK[verdict] > RANK[res["verdict"]] or (res["example"] is None and verdict != "PASS"):
                worst = max(diffs, default=None)
                res["example"] = {"vector": v, "tag": tags[v], "verdict": verdict,
                                  "inputs": operand_inputs(t, image(v), m),
                                  "diffs": [(d[2], d[3], d[4], d[5]) for d in sorted(diffs, reverse=True)[:4]],
                                  "worst": worst[2] if worst else "crash"}
            if RANK[verdict] > RANK[res["verdict"]]:
                res["verdict"] = verdict
            if tags[v] == "rand" and RANK[verdict] > RANK[res["clean"]]:
                res["clean"] = verdict
        c = per_class.setdefault(t["cls"], {"verdict": "PASS", "clean": "PASS", "tests": [], "uses": m["classes"][t["cls"]]["count"]})
        c["tests"].append((t, res))
        if RANK[res["verdict"]] > RANK[c["verdict"]]:
            c["verdict"] = res["verdict"]
        if RANK[res["clean"]] > RANK[c["clean"]]:
            c["clean"] = res["clean"]

    counts = collections.Counter(c["verdict"] for c in per_class.values())
    clean = collections.Counter(c["clean"] for c in per_class.values())
    skipped = sum(r["skipped"] for c in per_class.values() for _, r in c["tests"])
    lines = ["# Opcode conformance: OUR recompiled code vs PCSX2", "",
             f"{len(per_class)} classes ({len(m['tests'])} game words x {len(tags)} input vectors): "
             + ", ".join(f"{k} {counts[k]}" for k in RANK if counts[k])
             + "; on clean inputs (rand): " + ", ".join(f"{k} {clean[k]}" for k in RANK if clean[k]), "",
             f"PCSX2 run: {pinfo.get('seconds', '?')} s, complete={pinfo.get('complete', '?')}, "
             f"COP0 Status {pinfo.get('cop0_status', '?')}; records PCSX2 never ran: {skipped}; "
             f"records with k0/k1 changed under the test (interrupt): {corrupt}"
             + (f"; seeded bug: {m['seed_bug']}" if m.get("seed_bug") else ""), "",
             "Verdicts: FAIL = a value differs beyond 1 ulp; FLAG = only FCR31/STATUS/MAC/CLIP differ; "
             "ULP = <=2-ulp float rounding only; CRASH = our code faulted. 'clean' = worst verdict on the rand vectors.",
             "Vectors: rand = moderate random normal floats/ints, int = integer bit patterns in float registers, edge = 0/-0/denormal/max/int limits, exp255 = also "
             "Inf/NaN bit patterns (the PS2 has none: games only get them from bad data).", "",
             "| class | game uses | clean | all | bad records (rand/int/edge/exp255) | worst field |", "|---|---|---|---|---|---|"]
    order = sorted(per_class.items(), key=lambda kv: (-RANK[kv[1]["clean"]], -RANK[kv[1]["verdict"]], -kv[1]["uses"]))
    for cls, c in order:
        tg = collections.Counter()
        fl = collections.Counter()
        for _, r in c["tests"]:
            tg.update(r["tags"])
            fl.update(r["fields"])
        bad = f"{tg['rand']}/{tg['int']}/{tg['edge']}/{tg['exp255']}"
        worst = ", ".join(f"{k} x{n}" for k, n in fl.most_common(2)) or "-"
        lines.append(f"| {cls} | {c['uses']} | {c['clean']} | {c['verdict']} | {bad} | {worst} |")
    for cls, c in order:
        if c["verdict"] == "PASS":
            continue
        lines += ["", f"## {cls} ({c['verdict']}, {c['uses']} uses in SDBZ)"]
        for t, r in c["tests"]:
            if r["verdict"] == "PASS":
                lines.append(f"- `{t['text']}` ({t['word']}, {t['uses']} uses): PASS")
                continue
            ex = r["example"]
            lines.append(f"- `{t['text']}` ({t['word']}, {t['uses']} uses): {r['verdict']}, "
                         f"{sum(r['bad'].values())}/{len(tags)} records; e.g. vector {ex['vector']} ({ex['tag']}): "
                         f"{ex['inputs']}")
            for nm, x, y, isf in ex["diffs"]:
                lines.append(f"  - {nm}: ours {fmt(x) if isf else f'0x{x:08x}'}  pcsx2 {fmt(y) if isf else f'0x{y:08x}'}")
    rep = a.report or a.dir / "report.md"
    rep.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("all: " + " ".join(f"{k} {counts[k]}" for k in RANK if counts[k]) + "  clean: "
          + " ".join(f"{k} {clean[k]}" for k in RANK if clean[k]) + f"  (classes) -> {rep}")
    if skipped or corrupt:
        print(f"WARNING: {skipped} record(s) PCSX2 never ran, {corrupt} with k0/k1 changed under the test")


if __name__ == "__main__":
    main()
