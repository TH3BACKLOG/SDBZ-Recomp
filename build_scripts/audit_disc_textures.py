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
        img = bytes(d[img_off:img_off + img_sz])
        blob = img + bytes(d[clut_off:clut_off + clut_sz])
        yield {
            "imgMd5": hashlib.md5(img).hexdigest(),
            "imgHead": hashlib.md5(img[:1024]).hexdigest(),
            "w": w, "h": h, "mips": mips,
            "type": IMG_TYPE.get(img_t, f"?{img_t}"),
            "clutType": clut_t, "clutColors": ncol,
            "imgBytes": img_sz, "clutBytes": clut_sz,
            "md5": hashlib.md5(blob).hexdigest(),
        }
        if total == 0 or p + total > len(d):
            return
        p += total


# Bits per pixel of a host->local IMAGE transfer, by BITBLTBUF.DPSM.
_XFER_BPP = {0x00: 32, 0x01: 24, 0x02: 16, 0x0A: 16, 0x13: 8, 0x14: 4, 0x1B: 8, 0x24: 4, 0x2C: 4,
             0x30: 32, 0x31: 24, 0x32: 16, 0x3A: 16}


def gsr_uploads(path):
    """Host->local IMAGE uploads in a PS2X_GSCAP .gsr -> list of upload byte strings.
    IMAGE data that runs past the end of a transfer continues in the next one (as in ps2_gs_gpu)."""
    raw = open(path, "rb").read()
    _, count, regs_size, _payload, _ = struct.unpack_from("<5I", raw, 4)
    ia = 24 + regs_size
    pa = ia + count * 12
    idx = struct.unpack_from(f"<{count * 3}I", raw, ia)
    bitblt, trxreg = 0, 0
    cur, want, pending = None, 0, 0
    done = []

    def take(chunk):
        nonlocal cur
        if cur is None:
            return
        cur += chunk
        if len(cur) >= want:
            done.append(bytes(cur[:want]))
            cur = None

    for i in range(count):
        blob = raw[pa + idx[3 * i]: pa + idx[3 * i] + idx[3 * i + 1]]
        off, end = 0, len(blob)
        if pending:
            n = min(pending * 16, end)
            take(blob[:n])
            off, pending = n, pending - n // 16
        while off + 16 <= end:
            lo, hi = struct.unpack_from("<QQ", blob, off)
            off += 16
            nloop, flg = lo & 0x7FFF, (lo >> 58) & 3
            nreg = ((lo >> 60) & 0xF) or 16
            if flg == 0:
                regs = [(hi >> (4 * k)) & 0xF for k in range(nreg)]
                for _ in range(nloop):
                    for r in regs:
                        if off + 16 > end:
                            break
                        a, b = struct.unpack_from("<QQ", blob, off)
                        off += 16
                        if r != 0xE:
                            continue
                        reg = b & 0xFF
                        if reg == 0x50:
                            bitblt = a
                        elif reg == 0x52:
                            trxreg = a
                        elif reg == 0x53 and (a & 3) == 0:  # TRXDIR host->local starts an upload
                            bpp = _XFER_BPP.get((bitblt >> 56) & 0x3F, 32)
                            want = (trxreg & 0xFFF) * ((trxreg >> 32) & 0xFFF) * bpp // 8
                            cur = bytearray() if want else None
            elif flg == 1:
                off += ((nloop * nreg + 1) // 2) * 16
            else:
                n = min(nloop * 16, end - off)
                take(blob[off:off + n])
                off += n
                pending = nloop - n // 16
    return done


def coverage(pics, dirs):
    """Which disc pictures were uploaded during the captured tour(s)."""
    full, head = set(), set()
    nup = 0
    for d in dirs:
        for f in sorted(os.listdir(d)):
            if f.endswith(".gsr"):
                for up in gsr_uploads(os.path.join(d, f)):
                    nup += 1
                    full.add(hashlib.md5(up).hexdigest())
                    head.add(hashlib.md5(up[:1024]).hexdigest())
    seen_files = collections.defaultdict(lambda: [0, 0])
    for p in pics:
        hit = p["imgMd5"] in full or p["imgHead"] in head
        seen_files[p["file"]][0] += 1
        seen_files[p["file"]][1] += hit
    hit_pics = sum(v[1] for v in seen_files.values())
    print(f"uploads in captures: {nup} ({len(full)} distinct)")
    print(f"disc pictures uploaded at least once: {hit_pics}/{len(pics)}")
    touched = {k: v for k, v in seen_files.items() if v[1]}
    print(f".pix files touched: {len(touched)}/{len(seen_files)}")
    by_dir = collections.Counter(k.rsplit("/", 1)[0] if "/" in k else "." for k in seen_files)
    hit_dir = collections.Counter(k.rsplit("/", 1)[0] if "/" in k else "." for k in touched)
    print("never-touched .pix by folder (the screens the tour did not reach):")
    for folder, n in by_dir.most_common():
        if hit_dir[folder] < n:
            print(f"  {folder:<40} {n - hit_dir[folder]:>4}/{n} untouched")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--json")
    ap.add_argument("--other", action="store_true")
    ap.add_argument("--coverage", nargs="+", metavar="TOUR_DIR",
                    help="match IMAGE uploads in PS2X_GSCAP tour captures against the disc pictures")
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
    if a.coverage:
        coverage(ok, a.coverage)
    if a.json:
        with open(a.json + ".tmp", "w", encoding="utf-8") as f:
            json.dump(out, f)
        os.replace(a.json + ".tmp", a.json)
        print("wrote", a.json)


if __name__ == "__main__":
    main()
