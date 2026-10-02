#!/usr/bin/env python3
"""L3 of the missing-graphics audit: our uploaded textures vs PCSX2's texture dump.

PCSX2 names dumps with its own hash and stores palette-resolved RGBA, so hashes
cannot be compared. Instead both sides are reduced to a palette-independent
signature: every pixel is relabelled by order of first appearance, then hashed
together with the size. An indexed (T8) upload and the PCSX2 PNG decoded from
the same indices give the same signature as long as the palette has no
duplicate colours (PCSX2 side may merge classes; that case is reported as
COARSER and checked by refining ours through the PNG's colour classes).

Inputs
  --log   PS2X_TEXHASH_LOG jsonl (one line per transfer)
  --bins  PS2X_TEXHASH_DUMPDIR dir (<hash>_<w>x<h>_psm<N>.bin)
  --pcsx2 PCSX2 texture dump dir (*.png)
Only T8 (psm19) uploads are matched; CT32 uploads are CLUTs, T8H (psm20) is
listed as skipped. Matching is whole-texture; atlases that PCSX2 split into
several PNGs show as UNMATCHED and need sub-region matching.

Usage:
    python audit_texhash_vs_pcsx2.py [--json out.json] [--limit N]
"""
import argparse
import glob
import hashlib
import json
import os
import re
import sys
from collections import Counter, defaultdict

import numpy as np
from PIL import Image

ROOT = r"F:\SDBZ Recomp"
LOG = os.path.join(ROOT, "Logs", "texhash.jsonl")
BINS = os.path.join(ROOT, "Logs", "texdump")
PCSX2 = r"F:\SDBZ Textures\SLUS-21442\dumps"
BIN_RE = re.compile(r"^([0-9a-f]+)_(\d+)x(\d+)_psm(\d+)\.bin$")


def canon(a):
    """Relabel by first appearance; return (signature hex, class count)."""
    flat = a.reshape(-1)
    uniq, first, inv = np.unique(flat, return_index=True, return_inverse=True)
    order = np.argsort(np.argsort(first))
    lab = order[inv].astype(np.uint16)
    h = hashlib.md5(lab.tobytes() + bytes(f"{a.shape}", "ascii")).hexdigest()
    return h, len(uniq), lab


def png_labels(path):
    im = np.asarray(Image.open(path).convert("RGBA"))
    packed = im.view(np.uint32).reshape(im.shape[:2])
    return packed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=LOG)
    ap.add_argument("--bins", default=BINS)
    ap.add_argument("--pcsx2", default=PCSX2)
    ap.add_argument("--json")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    # PCSX2 side: signature -> [files]; also by size for the coarser check
    sig2png = defaultdict(list)
    by_size = defaultdict(list)
    files = sorted(glob.glob(os.path.join(a.pcsx2, "*.png")))
    if a.limit:
        files = files[: a.limit]
    for i, f in enumerate(files):
        try:
            px = png_labels(f)
        except Exception as e:  # noqa: BLE001
            print("png fail", f, e, file=sys.stderr)
            continue
        h, n, lab = canon(px)
        sig2png[h].append(os.path.basename(f))
        by_size[px.shape].append((os.path.basename(f), n, lab))
        if i % 500 == 0:
            print(f"  pcsx2 {i}/{len(files)}", file=sys.stderr)

    # our side: unique T8 uploads (first sighting of each hash)
    first = {}
    uses = Counter()
    for line in open(a.log, encoding="utf-8", errors="replace"):
        try:
            r = json.loads(line)
        except ValueError:
            continue
        uses[r["h64"]] += 1
        first.setdefault(r["h64"], r)

    res = []
    cnt = Counter()
    for h64, r in first.items():
        psm, w, h = r["dpsm"], r["w"], r["h"]
        if psm == 0 and (w, h) == (16, 16):
            cnt["clut-skip"] += 1
            continue
        if psm != 19:
            cnt[f"skip-psm{psm}"] += 1
            continue
        p = os.path.join(a.bins, f"{h64}_{w}x{h}_psm{psm}.bin")
        if not os.path.exists(p):
            cnt["no-bin"] += 1
            res.append({"h64": h64, "w": w, "h": h, "status": "NO-BIN"})
            continue
        d = np.fromfile(p, dtype=np.uint8)
        if d.size < w * h:
            cnt["short-bin"] += 1
            continue
        a2 = d[: w * h].reshape(h, w)
        sig, ncls, lab = canon(a2)
        hit = sig2png.get(sig)
        if hit:
            st = "MATCH"
            who = hit
        else:
            # coarser: same size, PNG classes <= ours, and PNG class is a
            # function of our class (our label determines PNG label)
            who = []
            for name, n, plab in by_size.get((h, w), []):
                if n > ncls:
                    continue
                m = {}
                ok = True
                for o, q in zip(lab.reshape(-1)[::7], plab.reshape(-1)[::7]):
                    if m.setdefault(o, q) != q:
                        ok = False
                        break
                if ok:
                    who.append(name)
            st = "COARSER" if who else "UNMATCHED"
        cnt[st] += 1
        res.append({"h64": h64, "w": w, "h": h, "uses": uses[h64], "status": st,
                    "pcsx2": who[:3]})

    print("our unique uploads:", len(first))
    print("results:", dict(cnt))
    for r in res:
        if r["status"] in ("UNMATCHED", "NO-BIN"):
            print("  ", r["status"], r["h64"], f"{r['w']}x{r['h']}", "uses", r.get("uses"))
    if a.json:
        with open(a.json + ".tmp", "w", encoding="utf-8") as f:
            json.dump(res, f)
        os.replace(a.json + ".tmp", a.json)


if __name__ == "__main__":
    main()
