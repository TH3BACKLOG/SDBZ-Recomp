#!/usr/bin/env python3
"""L1 of the missing-graphics audit: inventory every texture on the disc, offline.

ARKD files named *.pix are an FPIX/XPIX wrapper around standard TIM2 textures.
This walks INFO.DAT, reads each .pix from GAME.DAT, parses every TIM2 picture and
records name, size, format, and a content hash of image+CLUT. No build, no run.

Usage:
    python audit_disc_textures.py                # summary + format census
    python audit_disc_textures.py --json out.json  # full inventory
    python audit_disc_textures.py --other        # census of non-.pix file types
"""
import argparse
import collections
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arkd_toc  # noqa: E402

ROOT = r"F:\SDBZ Recomp\Super Dragon Ball Z ISO\Arcade Version\SDBZ ISO 2"
IMG_TYPE = {1: "16bpp", 2: "24bpp", 3: "32bpp", 4: "T4", 5: "T8"}


def parse_tim2(d, base):
    """Yield dicts for each picture of a TIM2 starting at d[base:]."""
    if d[base:base + 4] != b"TIM2":
        return
    n = struct.unpack_from("<H", d, base + 6)[0]
    fmt_id = d[base + 5]
    p = base + (0x80 if fmt_id == 1 else 0x10)
    for i in range(n):
        if p + 48 > len(d):
            yield {"err": "truncated header"}
            return
        total, clut_sz, img_sz, hdr_sz, ncol = struct.unpack_from("<IIIHH", d, p)
        pic_fmt, mips, clut_t, img_t, w, h = struct.unpack_from("<BBBBHH", d, p + 16)
        img_off = p + hdr_sz
        clut_off = img_off + img_sz
        if mips > 1:
            # mip headers sit between the picture header and the data; hash the
            # whole payload so a mismatch is still detected.
            pass
        blob = bytes(d[img_off:img_off + img_sz]) + bytes(d[clut_off:clut_off + clut_sz])
        yield {
            "w": w, "h": h, "mips": mips,
            "type": IMG_TYPE.get(img_t, f"?{img_t}"),
            "clutType": clut_t, "clutColors": ncol,
            "imgBytes": img_sz, "clutBytes": clut_sz,
            "md5": hashlib.md5(blob).hexdigest(),
        }
        if total == 0 or p + total > len(d):
            return
        p += total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--json")
    ap.add_argument("--other", action="store_true")
    a = ap.parse_args()

    toc = arkd_toc.Toc(os.path.join(a.root, "INFO.DAT"))
    names = toc.all_names()
    if a.other:
        c = collections.Counter(n.rsplit(".", 1)[-1].lower() for n in names)
        print(c.most_common())
        return
    out = []
    bad = []
    with open(os.path.join(a.root, "GAME.DAT"), "rb") as g:
        for n in sorted(names):
            if not n.lower().endswith(".pix"):
                continue
            r = toc.rec(names[n])
            g.seek(r["lbn"] * 2048)
            d = g.read(r["size"])
            found = 0
            pos = 0
            while True:
                k = d.find(b"TIM2", pos)
                if k < 0:
                    break
                for pic in parse_tim2(d, k):
                    pic["file"] = n
                    out.append(pic)
                    found += 1
                pos = k + 4
            if found == 0:
                bad.append((n, d[:8]))
    ok = [p for p in out if "err" not in p]
    print(f".pix files: {len({p['file'] for p in out})} with textures; {len(bad)} with none")
    print(f"pictures: {len(ok)}   unique content hashes: {len({p['md5'] for p in ok})}")
    print("by type:", collections.Counter(p["type"] for p in ok).most_common())
    print("mipmapped:", sum(p["mips"] > 1 for p in ok))
    print("errors:", len(out) - len(ok))
    print("size buckets:", collections.Counter(f"{p['w']}x{p['h']}" for p in ok).most_common(8))
    for n, h in bad[:10]:
        print("  no TIM2:", n, h)
    if a.json:
        with open(a.json + ".tmp", "w", encoding="utf-8") as f:
            json.dump(out, f)
        os.replace(a.json + ".tmp", a.json)
        print("wrote", a.json)


if __name__ == "__main__":
    main()
