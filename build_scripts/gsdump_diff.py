#!/usr/bin/env python3
"""Diff dump-side GS draw state against the live EE path's GS draw state, to
find where the live recomp run diverges from an authentic PCSX2 GS dump.

Stage 5.11 context: a real PCSX2 GS dump of the memory-card screen, replayed
through our own rasterizer, renders the box correctly. That proves the
rasterizer/blender is correct given correct input -- it does NOT prove the
live EE path emits the same TEX0/CLUT/draw state as the dump. This script
answers that directly: disagreement here is a live-path GS-EMISSION bug, not
a rasterizer bug (the rasterizer was already cleared by the replay test).

Two JSONL inputs, both one-JSON-object-per-draw with matching field names:
  dump side: python gsdump_draws.py dump.gs.zst --sprites --emit-jsonl dump.jsonl
  live side: run the game with PS2X_GSHISTORY_DUMP / PS2X_GSHISTORY_FRAMES set.

WHY THIS IS KEYED ON FORMAT, NOT ADDRESS
----------------------------------------
The first version of this script bucketed draws by (prim, tme, tbp0, cbp, psm)
and reported that the live path "never emits" 3 of the dump's 4 TEX0/CLUT
combos. That was an artifact. The two runs reach the same screen by different
paths, so the game allocates GS VRAM differently -- a constant shift in the
CLUT bases was enough to shatter every bucket into "only in dump" / "only in
live" and hide the fact that formats, draw counts and on-screen geometry all
agreed exactly.

So the primary key here is the FORMAT signature (prim, tme, psm, tbw, tw, th),
which is allocation-independent. tbp0/cbp are reported as VALUES inside a
matched bucket, as a dump->live address map. That turns "live never emits
this" into "live emits the same thing, from a different VRAM address" --
which is a finding rather than a false alarm.

Geometry (x0,y0,x1,y1) is captured on both sides and IS used: live bboxes
carry the raw XYOFFSET (a 512x448 framebuffer centered at 2048,2048 gives
1792,1824), dump bboxes are already offset-corrected. --xyoffset normalizes
both so draws can be matched by SHAPE rather than by an inferred address.

Frames are aligned by content, not guessed: --align scores every live frame's
format census against the dump's and picks the best match, so the live capture
window never has to be tuned by re-running the game.

Fields that hold one constant value across a whole side are reported as NOT
MEASURED rather than as divergences -- a constant column cannot distinguish
anything, and reporting it as a difference is a false positive.

Usage:
    python gsdump_diff.py dump.jsonl live.jsonl
    python gsdump_diff.py dump.jsonl live.jsonl --live-frame 249 --dump-frame 0
    python gsdump_diff.py dump.jsonl live.jsonl --find-rect 464 120
    python gsdump_diff.py dump.jsonl live.jsonl --all-frames
"""
import argparse
import collections
import json
import sys
from pathlib import Path

# Allocation-independent format signature. Two draws share a bucket iff they
# describe the same KIND of primitive sampling the same KIND of texture.
KEY_FIELDS = ("prim", "tme", "psm", "tbw", "tw", "th")

# Where the texture and its palette actually live. Deliberately NOT part of the
# key -- these are what we want to observe differing, not what should split
# buckets apart.
ADDR_FIELDS = ("tbp0", "cbp")

# Everything else worth comparing once a bucket matches.
VALUE_FIELDS = ("abe", "cpsm", "csm", "csa", "cld", "test", "alpha")

GEOM_FIELDS = ("x0", "y0", "x1", "y1")


def load_jsonl(path):
    records = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            records.append(json.loads(line))
    return records


def hexint(s, default=0):
    try:
        return int(str(s), 16)
    except (TypeError, ValueError):
        return default


def key_of(r):
    # With TME off the draw samples no texture, so TEX0 is whatever was left in
    # the register from an earlier draw. Keying an untextured draw on stale
    # texture state splits identical draws apart for no reason.
    if hexint(r.get("tme", "0x0")) == 0:
        return tuple(r.get(k, "0x0") if k in ("prim", "tme") else "-"
                     for k in KEY_FIELDS)
    return tuple(r.get(k, "0x0") for k in KEY_FIELDS)


def key_str(k):
    d = dict(zip(KEY_FIELDS, k))
    if d["psm"] == "-":
        return "prim=%s tme=0 (untextured)" % d["prim"]
    return ("prim=%s tme=%s psm=%s tbw=%s %sx%s"
            % (d["prim"], d["tme"], d["psm"], d["tbw"],
               hexint(d["tw"]), hexint(d["th"])))


def census(records):
    return collections.Counter(key_of(r) for r in records)


def frames_of(records):
    """Group records by frame index (int), preserving order within a frame."""
    out = collections.OrderedDict()
    for r in records:
        out.setdefault(hexint(r.get("frame", "0x0")), []).append(r)
    return out


def detect_xyoffset(records):
    """The XYOFFSET a side's coordinates are expressed relative to.

    Live capture records raw GS pixel coords including XYOFFSET; the dump-side
    decoder already subtracts it. Taking each side's own minimum x0/y0 recovers
    the offset for both cases (a side that is already corrected yields 0,0).
    """
    if not records:
        return 0.0, 0.0
    # Rounded to whole pixels: a stray sub-pixel vertex must not shift every
    # rect on that side by half a texel.
    return (float(round(min(float(r.get("x0", 0.0)) for r in records))),
            float(round(min(float(r.get("y0", 0.0)) for r in records))))


def rect(r, ox, oy):
    """(w, h, x, y) in screen pixels, XYOFFSET removed."""
    x0, y0 = float(r.get("x0", 0.0)) - ox, float(r.get("y0", 0.0)) - oy
    x1, y1 = float(r.get("x1", 0.0)) - ox, float(r.get("y1", 0.0)) - oy
    return (round(x1 - x0), round(y1 - y0), round(x0), round(y0))


def constant_columns(records, fields):
    """Fields holding exactly one value across every record -- unmeasured."""
    if not records:
        return set(fields)
    return {f for f in fields if len({r.get(f) for r in records}) == 1}


def similarity(a, b):
    """Multiset overlap of two format censuses: |A n B| / |A u B|."""
    keys = set(a) | set(b)
    if not keys:
        return 0.0
    inter = sum(min(a.get(k, 0), b.get(k, 0)) for k in keys)
    union = sum(max(a.get(k, 0), b.get(k, 0)) for k in keys)
    return inter / union if union else 0.0


def pick_dump_frame(dump_frames):
    """Densest dump frame (dumps repeat a static screen; take the fullest)."""
    return max(dump_frames.items(), key=lambda kv: (len(kv[1]), -kv[0]))[0]


def align(dump_recs, live_frames, top=5):
    """Rank live frames by how well their format census matches the dump's."""
    target = census(dump_recs)
    scored = [(similarity(target, census(recs)), fi, len(recs))
              for fi, recs in live_frames.items()]
    scored.sort(key=lambda t: (-t[0], t[1]))
    return scored[:top], scored


# ---------------------------------------------------------------------------


def report_find_rect(w, h, dump_recs, live_recs, dox, doy, lox, loy):
    print("== draws matching shape %dx%d ==" % (w, h))
    for name, recs, ox, oy in (("dump", dump_recs, dox, doy),
                               ("live", live_recs, lox, loy)):
        hits = [(r, rect(r, ox, oy)) for r in recs]
        hits = [(r, g) for r, g in hits if g[0] == w and g[1] == h]
        if not hits:
            print("  %s: no draw of that shape" % name)
            continue
        pos = collections.Counter((g[2], g[3]) for _, g in hits)
        print("  %s: %d draws" % (name, len(hits)))
        for (x, y), n in pos.most_common(8):
            sample = next(r for r, g in hits if (g[2], g[3]) == (x, y))
            frs = sorted({hexint(r["frame"]) for r, g in hits
                          if (g[2], g[3]) == (x, y)})
            span = ("frame %d" % frs[0] if len(frs) == 1
                    else "frames %d..%d" % (frs[0], frs[-1]))
            print("      @(%d,%d) n=%d  %s  tbp0=%s cbp=%s psm=%s  %s"
                  % (x, y, n, key_str(key_of(sample)),
                     sample.get("tbp0"), sample.get("cbp"),
                     sample.get("psm"), span))
    print()


def report_buckets(dump_sel, live_sel, dox, doy, lox, loy, dead):
    d_b, l_b = collections.OrderedDict(), collections.OrderedDict()
    for r in dump_sel:
        d_b.setdefault(key_of(r), []).append(r)
    for r in live_sel:
        l_b.setdefault(key_of(r), []).append(r)

    both = [k for k in d_b if k in l_b]
    only_d = [k for k in d_b if k not in l_b]
    only_l = [k for k in l_b if k not in d_b]

    print("== format buckets: %d matched, %d dump-only, %d live-only =="
          % (len(both), len(only_d), len(only_l)))
    print()

    for k in sorted(both, key=lambda k: -len(d_b[k])):
        d, l = d_b[k], l_b[k]
        textured = hexint(dict(zip(KEY_FIELDS, k))["tme"]) != 0
        print("  [MATCH] %s" % key_str(k))
        print("      draws        dump=%-6d live=%d" % (len(d), len(l)))

        # dump->live address map: the actual finding when VRAM layout differs.
        for f in (ADDR_FIELDS if textured else ()):
            dv = collections.Counter(r.get(f) for r in d)
            lv = collections.Counter(r.get(f) for r in l)
            dtop, ltop = dv.most_common(1)[0][0], lv.most_common(1)[0][0]
            if dtop == ltop and len(dv) == 1 and len(lv) == 1:
                note = "same"
            else:
                delta = hexint(ltop) - hexint(dtop)
                note = "REMAPPED  d=%s%#X" % ("-" if delta < 0 else "+", abs(delta))
            extra = ""
            if len(dv) > 1 or len(lv) > 1:
                extra = "   (dump %d distinct, live %d distinct)" % (len(dv), len(lv))
            print("      %-12s dump=%-8s live=%-8s %s%s"
                  % (f, dtop, ltop, note, extra))

        # geometry, XYOFFSET-normalized, so identical shapes read as identical
        dg = collections.Counter(rect(r, dox, doy)[:2] for r in d)
        lg = collections.Counter(rect(r, lox, loy)[:2] for r in l)
        agree = set(dg) == set(lg)
        print("      bbox sizes   dump=%s" % _fmt_sizes(dg))
        print("                   live=%s   %s"
              % (_fmt_sizes(lg), "identical" if agree else "DIFFER"))

        # field-level divergence over every draw, not just the first. Texture
        # and CLUT fields are stale register contents when TME is off, so they
        # are meaningless there -- reporting them is the same false-positive
        # class that made 'alpha' look like a finding.
        skip = dead if textured else (dead | {"cpsm", "csm", "csa", "cld"})
        live_fields = [f for f in VALUE_FIELDS if f not in skip]
        for f in live_fields:
            dv = {r.get(f) for r in d}
            lv = {r.get(f) for r in l}
            if dv != lv:
                print("      %-12s dump=%-16s live=%s"
                      % (f, ",".join(sorted(dv)), ",".join(sorted(lv))))
        print()

    for name, keys, buckets in (("dump", only_d, d_b), ("live", only_l, l_b)):
        if not keys:
            continue
        print("  buckets ONLY in %s:" % name)
        for k in sorted(keys, key=lambda k: -len(buckets[k])):
            sample = buckets[k][0]
            print("      %s  n=%d  tbp0=%s cbp=%s"
                  % (key_str(k), len(buckets[k]),
                     sample.get("tbp0"), sample.get("cbp")))
        print()


def _fmt_sizes(counter, limit=5):
    return " ".join("%dx%d:%d" % (w, h, n)
                    for (w, h), n in counter.most_common(limit))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump_jsonl", type=Path,
                    help="output of gsdump_draws.py --emit-jsonl")
    ap.add_argument("live_jsonl", type=Path,
                    help="output of the live PS2X_GSHISTORY_DUMP run")
    ap.add_argument("--dump-frame", type=int,
                    help="dump frame index (default: densest frame)")
    ap.add_argument("--live-frame", type=int,
                    help="live frame index (default: best content match)")
    ap.add_argument("--all-frames", action="store_true",
                    help="compare whole files instead of aligning one frame")
    ap.add_argument("--xyoffset", default="auto",
                    help="'auto' (default), 'none', or 'X,Y' applied to BOTH sides")
    ap.add_argument("--find-rect", nargs=2, type=int, metavar=("W", "H"),
                    help="locate draws by shape on both sides, then exit")
    ap.add_argument("--top", type=int, default=5,
                    help="how many candidate live frames to list (default 5)")
    args = ap.parse_args()

    dump_recs = load_jsonl(args.dump_jsonl)
    live_recs = load_jsonl(args.live_jsonl)
    if not dump_recs or not live_recs:
        print("error: one side is empty (dump=%d live=%d)"
              % (len(dump_recs), len(live_recs)), file=sys.stderr)
        return 1

    if args.xyoffset == "auto":
        dox, doy = detect_xyoffset(dump_recs)
        lox, loy = detect_xyoffset(live_recs)
    elif args.xyoffset == "none":
        dox = doy = lox = loy = 0.0
    else:
        x, y = args.xyoffset.split(",")
        dox = lox = float(x)
        doy = loy = float(y)

    dump_frames = frames_of(dump_recs)
    live_frames = frames_of(live_recs)

    print("dump: %d draws, %d frames, %d format buckets, xyoffset=(%g,%g)"
          % (len(dump_recs), len(dump_frames), len(census(dump_recs)), dox, doy))
    print("live: %d draws, %d frames, %d format buckets, xyoffset=(%g,%g)"
          % (len(live_recs), len(live_frames), len(census(live_recs)), lox, loy))
    print()

    if args.find_rect:
        report_find_rect(args.find_rect[0], args.find_rect[1],
                         dump_recs, live_recs, dox, doy, lox, loy)
        return 0

    # --- degenerate-column guard -------------------------------------------
    # A field with one value across a whole side cannot distinguish anything.
    # Reporting it as a divergence is a false positive (this is exactly how
    # 'alpha dump=0x0 live=0x44' got reported as a finding when neither side
    # was measuring it).
    all_fields = ADDR_FIELDS + VALUE_FIELDS
    dead_d = constant_columns(dump_recs, all_fields)
    dead_l = constant_columns(live_recs, all_fields)
    dead = dead_d & dead_l
    if dead:
        print("== NOT MEASURED (single constant value on both sides) ==")
        for f in sorted(dead):
            print("  %-8s dump=%-10s live=%s"
                  % (f, dump_recs[0].get(f), live_recs[0].get(f)))
        print("  These are excluded from divergence reporting.")
        print()
    for name, s, other in (("dump", dead_d - dead, dead_l),
                           ("live", dead_l - dead, dead_d)):
        if s:
            print("  note: constant on %s only, varies on the other side: %s"
                  % (name, ", ".join(sorted(s))))
    if (dead_d - dead) or (dead_l - dead):
        print()

    # --- frame selection ----------------------------------------------------
    if args.all_frames:
        dump_sel, live_sel = dump_recs, live_recs
        print("== comparing WHOLE FILES (--all-frames) ==")
        print()
    else:
        df = args.dump_frame if args.dump_frame is not None \
            else pick_dump_frame(dump_frames)
        if df not in dump_frames:
            print("error: dump frame %d not present" % df, file=sys.stderr)
            return 1
        dump_sel = dump_frames[df]

        if args.live_frame is not None:
            lf = args.live_frame
            if lf not in live_frames:
                print("error: live frame %d not present" % lf, file=sys.stderr)
                return 1
            print("== frame selection: dump frame %d, live frame %d (explicit) =="
                  % (df, lf))
        else:
            top, _ = align(dump_sel, live_frames, args.top)
            lf = top[0][1]
            print("== frame auto-alignment: dump frame %d vs live frames ==" % df)
            for score, fi, n in top:
                print("      live frame %-5d %3d draws   match=%.3f%s"
                      % (fi, n, score, "   <- selected" if fi == lf else ""))
            print()
        live_sel = live_frames[lf]
        print("   dump frame %d: %d draws     live frame %d: %d draws"
              % (df, len(dump_sel), lf, len(live_sel)))
        print()

    report_buckets(dump_sel, live_sel, dox, doy, lox, loy, dead)
    return 0


if __name__ == "__main__":
    sys.exit(main())
