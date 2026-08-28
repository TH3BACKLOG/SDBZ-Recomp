#!/usr/bin/env python3
"""Decode the ARKD archive TOC (INFO.DAT) on the host -- ground truth for the
Stage 5.9 asset-load investigation.

This is an offline replica of ARKD_DVD.IRX's TocDecodeAndIndex (@ module +0x2C0),
so it answers "what SHOULD the driver see?" without a build or a run. Compare its
output against the runtime's [ARKD:toc] probe: any disagreement is a runtime bug,
and agreement means the fault is downstream of the table.

Descramble (from the decompile): the first 16 bytes are the key; every byte from
offset 16 upward is nibble-swapped, bitwise-NOTed, then has the key byte at the
same intra-16-block index subtracted. The IRX skips this entirely when the first
byte is zero, so an all-zero buffer silently stays all-zero.

Record layout (confirmed 2026-08-05 by decoding the real file): 48-byte records.
Record 0 is a header whose +44 dword is the file count. Records 1..count hold the
NUL-terminated name at +0 (up to 44 bytes), the start LBN at +36, the length in
sectors at +40, and the length in BYTES at +44. Records are sorted by name, which
is what makes the driver's 27-slot first-letter bucket array valid.

Usage:
    python arkd_toc.py                      # summary + bucket ranges
    python arkd_toc.py --list [N]           # first N records (default 40)
    python arkd_toc.py dis/toon.pix ...     # look up specific names
    python arkd_toc.py --iso PATH ...       # use a different INFO.DAT
"""
import argparse
import collections
import struct
import sys

DEFAULT_INFO = (r"F:\SDBZ Recomp\Super Dragon Ball Z ISO\Core Files"
                r"\Runtime_loaded_content\INFO.DAT")

REC = 48
NAME_MAX = 44


def descramble(raw: bytes) -> bytearray:
    if not raw or raw[0] == 0:
        # Mirror the IRX's own gate rather than quietly decoding anyway -- an
        # all-zero buffer must stay all-zero or we would invent data the driver
        # would never have seen.
        return bytearray(raw)
    key = raw[:16]
    out = bytearray(raw)
    for o in range(16, len(raw)):
        b = raw[o]
        b = ((b << 4) | (b >> 4)) & 0xFF
        b = (~b) & 0xFF
        out[o] = (b - key[o & 15]) & 0xFF
    return out


class Toc:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.raw = f.read()
        self.dec = descramble(self.raw)
        self.count = self.u32(44)

    def u32(self, off):
        return struct.unpack_from("<I", self.dec, off)[0]

    def name(self, k):
        return self.dec[REC * k: REC * k + NAME_MAX].split(b"\0")[0].decode("latin1")

    def rec(self, k):
        base = REC * k
        return dict(idx=k, off=base, name=self.name(k),
                    lbn=self.u32(base + 36), sectors=self.u32(base + 40),
                    size=self.u32(base + 44))

    def all_names(self):
        return {self.name(k): k for k in range(1, self.count + 1)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="archive paths to look up")
    ap.add_argument("--iso", default=DEFAULT_INFO, help="path to INFO.DAT")
    ap.add_argument("--list", nargs="?", type=int, const=40, metavar="N",
                    help="dump the first N records")
    args = ap.parse_args(argv)

    try:
        toc = Toc(args.iso)
    except OSError as e:
        print(f"cannot read {args.iso}: {e}", file=sys.stderr)
        return 2

    print(f"{args.iso}")
    print(f"  {len(toc.raw)} bytes ({len(toc.raw) // 2048} sectors), "
          f"count={toc.count}, table end=0x{REC * toc.count + REC:x}")
    if not 0 < toc.count < 0x800:
        print("  !! count implausible -- descramble or layout is wrong")
        return 1

    if args.list is not None:
        for k in range(1, min(toc.count, args.list) + 1):
            r = toc.rec(k)
            print(f"  [{r['idx']:>4}] @0x{r['off']:05x} lbn={r['lbn']:<8} "
                  f"sect={r['sectors']:<6} size={r['size']:<10} {r['name']}")

    names = toc.all_names()

    if args.names:
        for want in args.names:
            k = names.get(want)
            if k is None:
                print(f"  MISS  {want}")
                continue
            r = toc.rec(k)
            print(f"  [{r['idx']:>4}] @0x{r['off']:05x} lbn={r['lbn']:<8} "
                  f"sect={r['sectors']:<6} size={r['size']:<10} {want}")
        return 0

    # Bucket ranges. TocLookup scans records [bucket[c] .. bucket[c+1]), so these
    # are exactly the index bounds the runtime's dword_BEE0 array must reproduce.
    first = collections.Counter(n[0] for n in names)
    print("  first-letter buckets (index ranges TocLookup must scan):")
    lo = 1
    for ch in sorted(first):
        print(f"    '{ch}'  [{lo}, {lo + first[ch]})   {first[ch]} files")
        lo += first[ch]

    # Sanity: a zero size would be the smoking gun if it existed on disk.
    zeros = [k for k in range(1, toc.count + 1) if toc.u32(REC * k + 44) == 0]
    print(f"  records with size==0: {len(zeros)}"
          + (f"  {zeros[:10]}" if zeros else "  (none -- disk data is clean)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
