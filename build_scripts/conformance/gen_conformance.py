#!/usr/bin/env python3
"""gen_conformance.py -- Stage B opcode conformance test: build the test ELF both sides run.

Harvests the computational opcode classes SDBZ really uses (conf_isa.classify over every
func-map word of the game ELF), keeps up to 4 real game words per class (different shapes:
dest masks, register overlaps), and builds conformance.elf:

  test function = save ra -> load ALL state of the files the word touches from the input image
                  (k0) -> THE GAME WORD -> store that state to the output record (k1) -> return
  main (PCSX2 only) = di, then for each job {fn, in, out}: k0=in, k1=out, jalr fn; then DONE.

Our side runs the same functions recompiled by ps2_recomp (ps2x_conformance walks the same
job table), PCSX2 runs main. conformance_diff.py compares the two output regions.

    python gen_conformance.py                  -> out/ (elf, funcmap, toml, manifest) + ps2_recomp
    python gen_conformance.py --only sqrt.s,vaddx --vectors 4      (probe)
    python gen_conformance.py --seed-bug sqrt  (seeded control: our sqrt.s reads fs, like the 09-26 bug)
    python gen_conformance.py --game-fixes     (apply gen_sqrt_abs_overrides.fix_source, as the game overrides do)
"""
import argparse
import collections
import csv
import json
import os
import random
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import conf_isa as I  # noqa: E402
from mips_r5900_disassembler import load_elf, read_word  # noqa: E402
import gen_sqrt_abs_overrides as SQ  # noqa: E402

GAME_ELF = ROOT / "ELF" / "SLUS_214.42"
FUNC_MAP = ROOT / "build_scripts" / "funcmap" / "sdbz_func_map_regen.csv"
# The game's runner (output/, 09-26 22:19) came from this exe (09-26 21:36): test the same translator.
RECOMP_EXE = ROOT / "build" / "ps2xRecomp" / "RelWithDebInfo" / "ps2_recomp.exe"

# ---- memory map (shared with conformance_main.cpp and conformance_diff.py via manifest.json)
TEXT_BASE = 0x00100000
HDR = 0x00400000          # header: see HDR_* offsets
HDR_MAGIC, HDR_JOBS, HDR_NJOBS, HDR_OUT, HDR_OUTSIZE, HDR_PROGRESS, HDR_DONE, HDR_STATUS = 0, 8, 12, 16, 20, 24, 28, 32
MAGIC = 0x464E4F43        # 'CONF'
DONE = 0x600DF00D
IN_BASE = HDR + 0x100
OUT_BASE = 0x00800000
OUT_LIMIT = 0x01F00000

# ---- state image layout (input image and output record use the same group layouts)
G_SIZE, F_SIZE, V_SIZE = 560, 144, 656   # G: gpr[32]x16, HI@512, LO@528, SA@544
G_OFF, F_OFF, V_OFF = 0, 560, 704        # F: f[32]x4, FCR31@128, ACC@132 / V: vf[32]x16, ctrl[32]x4 @512, ACC@640
IMG_SIZE = 1360
REC_HDR = 16                             # rec[0]=saved ra, [4]=k0 seen, [8]=k1 seen, [12]=ours crash flag
GROUP_SIZE = {"G": G_SIZE, "F": F_SIZE, "V": V_SIZE}
GROUP_IN_OFF = {"G": G_OFF, "F": F_OFF, "V": V_OFF}

AT, K0, K1, RA = 1, I.K0, I.K1, 31


# ------------------------------------------------------------------ harvest
def harvest(per_class):
    segs, _ = load_elf(GAME_ELF)
    addrs = set()
    with open(FUNC_MAP, newline="") as fh:
        for row in csv.DictReader(fh):
            addrs.update(range(int(row["start"], 16), int(row["end"], 16), 4))
    words = collections.defaultdict(collections.Counter)
    ops = {}
    skipped = collections.Counter()
    for a in sorted(addrs):
        w = read_word(segs, a)
        if w is None:
            continue
        r = I.classify(w)
        if isinstance(r, str):
            skipped[r] += 1
            continue
        words[r.cls][w] += 1
        ops[w] = r
    classes = {}
    for cls, cnt in words.items():
        pick, shapes = [], set()
        for w, _ in cnt.most_common():           # one word per shape, most used first
            if ops[w].shape not in shapes:
                shapes.add(ops[w].shape)
                pick.append(w)
            if len(pick) == per_class:
                break
        for w, _ in cnt.most_common():           # fill up with the most used words
            if len(pick) == per_class:
                break
            if w not in pick:
                pick.append(w)
        classes[cls] = {"count": sum(cnt.values()), "groups": ops[pick[0]].groups,
                        "words": [{"word": f"0x{w:08x}", "text": ops[w].text, "uses": cnt[w]} for w in pick]}
    return classes, skipped


# ------------------------------------------------------------------ input vectors
F_EDGE = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x40000000, 0x00800000, 0x80800000,
          0x00000001, 0x007FFFFF, 0x807FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0x7E800000, 0x01000000, 0x4B800000,
          0xCF000000, 0x4F000000, 0x3EAAAAAB, 0x447A0000]
F_EXP255 = [0x7F800000, 0xFF800000, 0x7FC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x7F800001, 0xFFC00000]
I_EDGE = [0x00000000, 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x0000FFFF, 0x00008000, 0x00007FFF,
          0x00010000, 0x80008000, 0x7FFF7FFF, 0xFF00FF00, 0x00FF00FF, 0x80808080, 0x7F7F7F7F, 0x0000001F,
          0x00000020, 0x0000003F]


def f_rand(rng):
    """A normal float (no denormal, no exp255): moderate magnitude so products do not overflow."""
    if rng.random() < 0.2:                        # small integer valued float
        return struct.unpack("<I", struct.pack("<f", float(rng.randint(-300, 300))))[0]
    e = rng.randint(127 - 24, 127 + 24)
    return (rng.getrandbits(1) << 31) | (e << 23) | rng.getrandbits(23)


def f_int(rng):
    """An integer bit pattern in a float register (cvt.s.w / vitof inputs). Negative ones read as exp255."""
    return rng.choice([rng.randint(-(1 << 24), 1 << 24), rng.randint(-300, 300), rng.getrandbits(32)]) & 0xFFFFFFFF


def i_rand(rng):
    r = rng.random()
    if r < 0.25:
        return rng.randint(-200, 200) & 0xFFFFFFFF
    if r < 0.4:
        return rng.randint(0, 0xFFFF)
    return rng.getrandbits(32)


def make_vector(rng, tag):
    def fv():
        if tag == "int":
            return f_int(rng)
        if tag == "edge" and rng.random() < 0.6:
            return rng.choice(F_EDGE)
        if tag == "exp255" and rng.random() < 0.35:
            return rng.choice(F_EXP255 + F_EDGE)
        return f_rand(rng)

    def iv():
        if tag != "rand" and rng.random() < 0.6:
            return rng.choice(I_EDGE)
        return i_rand(rng)

    img = bytearray(IMG_SIZE)
    # G: GPRs (mix float and int lanes so mfc1/qmtc2 see both), HI, LO, SA
    for r in range(32):
        lanes = [fv() if rng.random() < 0.25 else iv() for _ in range(4)]
        struct.pack_into("<4I", img, G_OFF + 16 * r, *lanes)
    for off in (512, 528):
        struct.pack_into("<4I", img, G_OFF + off, *[iv() for _ in range(4)])
    struct.pack_into("<I", img, G_OFF + 544, rng.randint(0, 15))
    # F: FPRs, FCR31 (C bit only: sticky flags start clear), ACC
    for r in range(32):
        struct.pack_into("<I", img, F_OFF + 4 * r, fv())
    struct.pack_into("<I", img, F_OFF + 128, rng.choice([0, 0x00800000]))
    struct.pack_into("<I", img, F_OFF + 132, f_rand(rng))
    # V: VF1-31 (vf0 is constant), VI1-15, CLIP, R, I, Q, ACC (status starts clear)
    for r in range(1, 32):
        struct.pack_into("<4I", img, V_OFF + 16 * r, *[fv() for _ in range(4)])
    for n in range(1, 16):
        struct.pack_into("<I", img, V_OFF + 512 + 4 * n, iv() & 0xFFFF)
    struct.pack_into("<I", img, V_OFF + 512 + 4 * 18, rng.getrandbits(24))
    struct.pack_into("<I", img, V_OFF + 512 + 4 * 20, 0x3F800000 | rng.getrandbits(23))
    struct.pack_into("<I", img, V_OFF + 512 + 4 * 21, f_rand(rng))
    struct.pack_into("<I", img, V_OFF + 512 + 4 * 22, f_rand(rng))
    struct.pack_into("<4I", img, V_OFF + 640, *[f_rand(rng) for _ in range(4)])
    return bytes(img)


# ------------------------------------------------------------------ test function
def test_body(word, groups, cls):
    c = [itype_sw(RA, K1, 0)]
    if "V" in groups:   # ACC first (vmula sets flags), then control regs, then VF
        c += [I.itype(I.LQC2, K0, 1, V_OFF + 640), I.vmulaw(0xF, 1, 0)]
        for n in I.VI_LOAD:
            c += [I.itype(I.LW, K0, AT, V_OFF + 512 + 4 * n), I.ctc2(AT, n)]
        c += [I.itype(I.LQC2, K0, r, V_OFF + 16 * r) for r in range(1, 32)]
    if "F" in groups:   # ACC = in * 1.0, then FCR31, then FPRs
        c += [I.itype(I.LWC1, K0, 0, F_OFF + 132), I.lui(AT, 0x3F80), I.mtc1(AT, 1), I.mula_s(0, 1),
              I.itype(I.LW, K0, AT, F_OFF + 128), I.ctc1(AT, 31)]
        c += [I.itype(I.LWC1, K0, r, F_OFF + 4 * r) for r in range(32)]
    if "G" in groups:   # HI, LO, SA through $at, then every GPR but zero/k0/k1
        # HI/LO as 64-bit halves (mthi/mthi1/...): our pmthi is 32-bit and pmfhi 64-bit (10-08 run 1; unused by SDBZ)
        for off, lo_op, hi_op in ((G_OFF + 512, I.mthi, I.mthi1), (G_OFF + 528, I.mtlo, I.mtlo1)):
            c += [I.itype(I.LD, K0, AT, off), lo_op(AT), I.itype(I.LD, K0, AT, off + 8), hi_op(AT)]
        c += [I.itype(I.LQ, K0, AT, G_OFF + 544), I.mtsa(AT)]
        c += [I.itype(I.LQ, K0, r, G_OFF + 16 * r) for r in range(1, 32) if r not in (K0, K1)]
    c.append(word)
    if cls in I.Q_WRITERS:
        c.append(I.VWAITQ)
    out = REC_HDR
    for g in "GFV":
        if g not in groups:
            continue
        if g == "G":
            c += [I.itype(I.SQ, K1, r, out + 16 * r) for r in range(32)]
            for off, lo_op, hi_op in ((out + 512, I.mfhi, I.mfhi1), (out + 528, I.mflo, I.mflo1)):
                c += [lo_op(AT), I.itype(I.SD, K1, AT, off), hi_op(AT), I.itype(I.SD, K1, AT, off + 8)]
            c += [I.mfsa(AT), I.itype(I.SW, K1, AT, out + 544)]
        elif g == "F":
            c += [I.itype(I.SWC1, K1, r, out + 4 * r) for r in range(32)]
            c += [I.cfc1(AT, 31), I.itype(I.SW, K1, AT, out + 128),
                  I.mtc1(0, 0), I.madd_s(0, 0, 0), I.itype(I.SWC1, K1, 0, out + 132)]   # f0 = ACC + 0*0
        else:
            c += [I.itype(I.SQC2, K1, r, out + 16 * r) for r in range(32)]
            for n in I.VI_STORE:
                c += [I.cfc2(AT, n), I.itype(I.SW, K1, AT, out + 512 + 4 * n)]
            c += [I.vmaddx(0xF, 1, 0, 0), I.itype(I.SQC2, K1, 1, out + 640)]          # vf1 = ACC + vf0*0
        out += GROUP_SIZE[g]
    c += [itype_sw(K0, K1, 4), itype_sw(K1, K1, 8), I.itype(I.LW, K1, RA, 0), I.JR_RA, I.NOP]
    return c, out


def itype_sw(rt, base, off):
    return I.itype(I.SW, base, rt, off)


def main_code(use_di):
    """PCSX2 driver. Keeps its loop state in the header: tests clobber every GPR but k0/k1."""
    t0, t1, t2, t3, t4, t5 = 8, 9, 10, 11, 12, 13
    c = [I.DI] if use_di else []
    loop = len(c)
    c += [I.lui(t0, HDR >> 16),
          I.itype(I.LW, t0, t1, HDR_PROGRESS),
          I.itype(I.LW, t0, t2, HDR_NJOBS)]
    beq_done = len(c)
    c += [None, I.NOP,
          I.itype(I.LW, t0, t3, HDR_JOBS),
          I.sll(t4, t1, 4),
          I.addu(t3, t3, t4),
          I.addiu(t1, t1, 1),
          I.itype(I.SW, t0, t1, HDR_PROGRESS),
          I.itype(I.LW, t3, t5, 0),
          I.itype(I.LW, t3, K0, 4),
          I.itype(I.LW, t3, K1, 8),
          I.jalr(t5), I.NOP]
    c += [I.beq(0, 0, loop - len(c) - 1), I.NOP]
    done = len(c)
    c[beq_done] = I.beq(t1, t2, done - beq_done - 1)
    c += [I.lui(t0, HDR >> 16), I.lui(t1, DONE >> 16), I.ori(t1, t1, DONE & 0xFFFF),
          I.itype(I.SW, t0, t1, HDR_DONE),
          (0x10 << 26) | (t2 << 16) | (12 << 11),            # mfc0 t2, Status (after DONE: a trap here is harmless)
          I.itype(I.SW, t0, t2, HDR_STATUS)]
    c += [I.beq(0, 0, -1), I.NOP]
    return c


# ------------------------------------------------------------------ ELF writer
def write_elf(path, entry, text, data):
    """Minimal MIPS EXEC ELF: 2 PT_LOAD segments + .text/.data/.shstrtab sections."""
    shstr = b"\0.text\0.data\0.shstrtab\0"
    text_off = 0x100
    data_off = (text_off + len(text) + 15) & ~15
    shstr_off = data_off + len(data)
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    eh = bytearray(52)
    eh[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", eh, 16, 2, 8, 1, entry, 52, sh_off, 0x20924001, 52, 32, 2, 40, 4, 3)
    ph = struct.pack("<8I", 1, text_off, TEXT_BASE, TEXT_BASE, len(text), len(text), 5, 16)
    ph += struct.pack("<8I", 1, data_off, HDR, HDR, len(data), len(data), 6, 16)
    sh = bytes(40)
    sh += struct.pack("<10I", 1, 1, 6, TEXT_BASE, text_off, len(text), 0, 0, 16, 0)
    sh += struct.pack("<10I", 7, 1, 3, HDR, data_off, len(data), 0, 0, 16, 0)
    sh += struct.pack("<10I", 13, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    blob = bytearray(sh_off + len(sh))
    blob[0:52] = eh
    blob[52:52 + len(ph)] = ph
    blob[text_off:text_off + len(text)] = text
    blob[data_off:data_off + len(data)] = data
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    blob[sh_off:] = sh
    path.write_bytes(blob)


def safe(cls):
    return re.sub(r"[^a-z0-9]", "_", cls.lower())


# ------------------------------------------------------------------ build
def build(a):
    out = a.out
    out.mkdir(parents=True, exist_ok=True)
    classes, skipped = harvest(a.per_class)
    if a.only:
        want = set(a.only.split(","))
        missing = want - set(classes)
        if missing:
            sys.exit(f"--only: no such class in SDBZ: {sorted(missing)}")
        classes = {k: v for k, v in classes.items() if k in want}
    rng = random.Random(a.seed)
    n_int, n_edge, n_255 = max(1, a.vectors // 10), a.vectors // 4, max(1, a.vectors // 8)
    tags = ["rand"] * (a.vectors - n_int - n_edge - n_255) + ["int"] * n_int + ["edge"] * n_edge + ["exp255"] * n_255
    vectors = [make_vector(rng, t) for t in tags]

    words = []                    # text words
    main = main_code(not a.no_di)
    words += main
    words += [I.NOP] * ((-len(words)) % 4)
    tests = []
    for cls in sorted(classes):
        info = classes[cls]
        for k, wd in enumerate(info["words"]):
            body, rec = test_body(int(wd["word"], 16), info["groups"], cls)
            addr = TEXT_BASE + 4 * len(words)
            tests.append({"idx": len(tests), "name": f"ct{len(tests):04d}_{safe(cls)}_{k}", "cls": cls,
                          "word": wd["word"], "text": wd["text"], "uses": wd["uses"], "groups": info["groups"],
                          "addr": addr, "end": addr + 4 * len(body), "rec_size": rec})
            words += body
            words += [I.NOP] * ((-len(words)) % 4)
    text = b"".join(struct.pack("<I", w) for w in words)

    # data: header | input images | job table
    jobs_addr = IN_BASE + IMG_SIZE * len(vectors)
    jobs_addr = (jobs_addr + 15) & ~15
    jobs, cur = [], OUT_BASE
    for t in tests:               # test-major: one test runs all vectors in a row
        t["out"] = cur
        for v in range(len(vectors)):
            jobs.append((t["addr"], IN_BASE + IMG_SIZE * v, cur, t["idx"]))
            cur += t["rec_size"]
    if cur > OUT_LIMIT:
        sys.exit(f"output region too big: 0x{cur - OUT_BASE:x} bytes (use fewer --vectors or --only)")
    data = bytearray(jobs_addr - HDR + 16 * len(jobs))
    struct.pack_into("<8I", data, 0, MAGIC, 1, jobs_addr, len(jobs), OUT_BASE, cur - OUT_BASE, 0, 0)
    for v, img in enumerate(vectors):
        data[IN_BASE - HDR + IMG_SIZE * v:IN_BASE - HDR + IMG_SIZE * (v + 1)] = img
    for j, job in enumerate(jobs):
        struct.pack_into("<4I", data, jobs_addr - HDR + 16 * j, *job)

    elf = out / "conformance.elf"
    write_elf(elf, TEXT_BASE, text, bytes(data))
    with open(out / "funcmap.csv", "w", newline="") as fh:
        fh.write("name,start,end,size\n")
        fh.write(f"conf_main,0x{TEXT_BASE:08x},0x{TEXT_BASE + 4 * len(main):08x},0x{4 * len(main):x}\n")
        for t in tests:
            fh.write(f"{t['name']},0x{t['addr']:08x},0x{t['end']:08x},0x{t['end'] - t['addr']:x}\n")
    gen = out / "gen"
    toml = out / "conformance.toml"
    toml.write_text("\n".join([
        "[general]", f'input = "{elf.as_posix()}"', f'ghidra_output = "{(out / "funcmap.csv").as_posix()}"',
        f'output = "{gen.as_posix()}"', "single_file_output = false", "low_memory_mode = false",
        "patch_syscalls = false", "patch_cop0 = true", "patch_cache = true", ""]))
    manifest = {"hdr": HDR, "hdr_offsets": {"magic": HDR_MAGIC, "jobs": HDR_JOBS, "njobs": HDR_NJOBS, "out": HDR_OUT,
                                            "outsize": HDR_OUTSIZE, "progress": HDR_PROGRESS, "done": HDR_DONE,
                                            "status": HDR_STATUS},
                "done_magic": DONE, "out_base": OUT_BASE, "out_size": cur - OUT_BASE, "in_base": IN_BASE,
                "img_size": IMG_SIZE, "rec_hdr": REC_HDR, "group_size": GROUP_SIZE, "group_in_off": GROUP_IN_OFF,
                "vectors": tags, "seed": a.seed, "use_di": not a.no_di, "seed_bug": a.seed_bug,
                "recomp_exe": str(RECOMP_EXE), "tests": tests,
                "classes": {k: {"count": v["count"], "groups": v["groups"]} for k, v in classes.items()},
                "skipped": dict(skipped.most_common())}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"{len(classes)} classes, {len(tests)} tests x {len(vectors)} vectors = {len(jobs)} jobs; "
          f"text {len(text) // 1024} KB, output {(cur - OUT_BASE) // 1024} KB -> {elf}")
    return manifest, toml, gen


FN_RE = re.compile(r"^void (\w+)\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\) \{", re.M)


def recompile(manifest, toml, gen):
    if gen.exists():               # only our own generated folder; stale tests must not be compiled in
        for f in gen.glob("*"):
            if f.is_file():
                f.unlink()
    r = subprocess.run([str(RECOMP_EXE), str(toml)], capture_output=True, text=True, errors="replace")
    (gen.parent / "ps2_recomp.log").write_text(r.stdout + r.stderr, encoding="utf-8")
    if r.returncode != 0:
        sys.exit(f"ps2_recomp failed rc={r.returncode}; see {gen.parent / 'ps2_recomp.log'}")
    by_addr = {}
    for f in gen.glob("*.cpp"):
        src = f.read_text(errors="replace")
        for m in FN_RE.finditer(src):
            am = re.search(r"_0x([0-9a-f]+)$", m.group(1))
            if am:
                by_addr[int(am.group(1), 16)] = (m.group(1), f.name)
    lines, missing = [], []
    for t in manifest["tests"]:
        hit = by_addr.get(t["addr"])
        if not hit:
            missing.append(t["name"])
            continue
        t["fn"], t["cpp"] = hit
        lines.append(f"CONF_FN({hit[0]}, 0x{t['addr']:08x}u, {t['idx']})")
    if missing:
        sys.exit(f"ps2_recomp emitted no function for {len(missing)} test(s), e.g. {missing[:5]}")
    (gen / "conformance_fns.inc").write_text("\n".join(lines) + "\n")
    print(f"ps2_recomp: {len(lines)} test functions -> {gen}")


def seed_sqrt_bug(manifest, gen):
    """Re-create the 09-26 translator bug in the generated code: sqrt.s reads fs instead of ft."""
    n = 0
    for t in manifest["tests"]:
        if t["cls"] != "sqrt.s":
            continue
        w = int(t["word"], 16)
        fs, ft = (w >> 11) & 31, (w >> 16) & 31
        if fs == ft:
            continue
        p = gen / t["cpp"]
        # the game word's translation is the only sqrt in the file: ctx->f[fd] = FPU_SQRT_S(ctx->f[ft]);
        new, k = re.subn(rf"FPU_SQRT_S\(ctx->f\[{ft}\]\)", f"FPU_SQRT_S(ctx->f[{fs}])", p.read_text())
        if k:
            p.write_text(new)
            n += k
    if not n:
        sys.exit("--seed-bug sqrt: found no sqrt of ctx->f[ft] to rewrite (generated code changed?)")
    print(f"seeded control: {n} sqrt.s translation(s) now read fs")


def apply_game_fixes(gen):
    """The same rewrite the game's sqrt overrides get (gen_sqrt_abs_overrides.fix_source), so the
    report shows whether that rewrite matches PCSX2."""
    tot = collections.Counter()
    for p in sorted(gen.glob("*.cpp")):
        src = p.read_text()
        if not any(k in src for k in SQ.NEEDLES):
            continue
        new, n = SQ.fix_source(src)
        i = new.index("// Function: ")
        p.write_text(new[:i] + "#include <cstring>\n" + SQ.FIX_DEFS + "\n" + new[i:])
        tot.update(n)
    if not tot:
        sys.exit("--game-fixes: no sqrt.s / vsqrt / vrsqrt translation found (generated code changed?)")
    print("game fixes applied: " + ", ".join(f"{k} {v}" for k, v in tot.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=HERE / "out")
    ap.add_argument("--per-class", type=int, default=4)
    ap.add_argument("--vectors", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", help="comma list of classes (probe runs)")
    ap.add_argument("--no-di", action="store_true", help="main does not disable interrupts")
    ap.add_argument("--no-recomp", action="store_true")
    ap.add_argument("--seed-bug", choices=["sqrt"])
    ap.add_argument("--game-fixes", action="store_true", help="apply the game's sqrt override rewrite")
    ap.add_argument("--list", action="store_true", help="print the classes and exit")
    a = ap.parse_args()
    if a.list:
        classes, skipped = harvest(a.per_class)
        for k in sorted(classes, key=lambda k: -classes[k]["count"]):
            print(f"{k:10} {classes[k]['groups']:3} {classes[k]['count']:7}  " + " | ".join(w["text"] for w in classes[k]["words"]))
        print("skipped:", dict(skipped.most_common()))
        return
    gen_dir = a.out / "gen"
    before = {f.name: (f.read_bytes(), f.stat().st_mtime_ns) for f in gen_dir.glob("*") if f.is_file()} if gen_dir.exists() else {}
    manifest, toml, gen = build(a)
    if not a.no_recomp:
        recompile(manifest, toml, gen)
        if a.seed_bug == "sqrt":
            seed_sqrt_bug(manifest, gen)
        if a.game_fixes:
            apply_game_fixes(gen)
        (a.out / "manifest.json").write_text(json.dumps(manifest, indent=1))
        # recompile() rewrites every file: give byte-identical ones their old mtime back, so MSBuild
        # recompiles only the unity batches whose files really changed (2-3 of 14 for a fix variant).
        kept = changed = 0
        for f in gen.glob("*"):
            old = before.get(f.name)
            if f.is_file() and old and f.read_bytes() == old[0]:
                os.utime(f, ns=(old[1], old[1]))
                kept += 1
            elif f.is_file():
                changed += 1
        print(f"generated files: {changed} changed, {kept} unchanged (mtime kept)")


if __name__ == "__main__":
    main()
