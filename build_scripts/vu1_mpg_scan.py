#!/usr/bin/env python3
"""Static VU1 microprogram census: every VIF MPG upload in the ELF vs the micro images we captured.

A scene we never reached may run VU1 code we never ran. This finds that code without playing:

    python build_scripts/vu1_mpg_scan.py                     # ELF\\SLUS_214.42 vs logs\\vu1recomp\\images
    python build_scripts/vu1_mpg_scan.py --elf X --images DIR --json out.json

MPG VIFcode: CMD (bits 24-30) = 0x4A, NUM (16-23, 0 = 256) 64-bit instructions, ADDR (0-15) in
64-bit units. The VIF wants MPG data 64-bit aligned, so the VIFcode sits at an odd word and the code
starts at the next (8-aligned) word. A candidate is kept only if it fits the 16 KB micro memory and
holds at least one upper instruction with the E bit (or continues another candidate's range).

Seen = the block's bytes appear at its micro address in at least one captured 16 KB image.
Output: Logs/vu1scan/report.md (+ --json).
"""
import argparse
import json
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MICRO = 16 * 1024


def load_segments(elf):
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"not an ELF: {elf}")
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _pa, p_filesz = struct.unpack_from("<IIIII", data, phoff + i * phentsize)
        if p_type == 1 and p_filesz:
            segs.append((p_vaddr, data[p_off:p_off + p_filesz]))
    return segs


def scan(segs):
    blocks = []
    for base, blob in segs:
        n = len(blob)
        for off in range(4, n - 4, 8):              # VIFcode at an odd word -> data 8-aligned
            code, = struct.unpack_from("<I", blob, off)
            if (code >> 24) & 0x7F != 0x4A:
                continue
            num = (code >> 16) & 0xFF or 256
            addr = (code & 0xFFFF) * 8
            size = num * 8
            start = off + 4
            if addr + size > MICRO or start + size > n:
                continue
            body = blob[start:start + size]
            ebits = sum(1 for k in range(0, size, 8) if (struct.unpack_from("<I", body, k + 4)[0] >> 30) & 1)
            blocks.append({"vaddr": base + off, "micro": addr, "num": num, "ebits": ebits,
                           "crc": f"{zlib.crc32(body):08x}", "body": body})
    # Keep blocks with a few E bits (a real program ends a handful of times: SDBZ's have 3-4).
    # MIPS .text words that happen to look like an MPG VIFcode give random E-bit counts
    # (~half the "upper" words), so a high ratio means code, not microcode.
    # Plus blocks that end exactly where a kept block starts (multi-part uploads).
    kept = [b for b in blocks if 0 < b["ebits"] <= max(4, b["num"] // 16)]
    starts = {(b["micro"]) for b in kept}
    kept += [b for b in blocks if not b["ebits"] and b["micro"] + b["num"] * 8 in starts]
    uniq = {}
    for b in kept:
        uniq.setdefault((b["micro"], b["crc"]), b)
    return sorted(uniq.values(), key=lambda b: (b["micro"], b["vaddr"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default=str(ROOT / "ELF" / "SLUS_214.42"))
    ap.add_argument("--images", default=str(ROOT / "logs" / "vu1recomp" / "images"))
    ap.add_argument("--json")
    a = ap.parse_args()

    blocks = scan(load_segments(Path(a.elf)))
    images = {p.stem: p.read_bytes() for p in sorted(Path(a.images).glob("*.bin")) if p.stat().st_size == MICRO}
    for b in blocks:
        lo, hi = b["micro"], b["micro"] + b["num"] * 8
        b["seen_in"] = [k for k, img in images.items() if img[lo:hi] == b["body"]]

    out = ROOT / "Logs" / "vu1scan"
    out.mkdir(parents=True, exist_ok=True)
    unseen = [b for b in blocks if not b["seen_in"]]
    lines = [f"# VU1 MPG census: {Path(a.elf).name} vs {len(images)} captured micro images", "",
             f"{len(blocks)} MPG blocks in the ELF, {len(blocks) - len(unseen)} seen, **{len(unseen)} never ran in a capture**", "",
             "| ELF vaddr | micro addr | instrs | E bits | crc | seen in |", "|---|---|---|---|---|---|"]
    for b in blocks:
        seen = ", ".join(b["seen_in"][:3]) + (" ..." if len(b["seen_in"]) > 3 else "") if b["seen_in"] else "**never**"
        lines.append(f"| 0x{b['vaddr']:06x} | 0x{b['micro']:04x} | {b['num']} | {b['ebits']} | {b['crc']} | {seen} |")
    (out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if a.json:
        Path(a.json).write_text(json.dumps([{k: v for k, v in b.items() if k != "body"} for b in blocks], indent=1),
                                encoding="utf-8")
    print(lines[2].replace("**", ""))
    for b in unseen:
        print(f"  never: vaddr 0x{b['vaddr']:06x} micro 0x{b['micro']:04x} instrs {b['num']} crc {b['crc']}")
    print(f"report: {out / 'report.md'}")


if __name__ == "__main__":
    main()
