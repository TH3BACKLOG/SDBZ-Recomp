#!/usr/bin/env python3
"""Decode KICKDATA records in a .vucap capture into readable GS vertices.

Every other vucap tool treats KICKDATA as an opaque byte blob (vucap.py just
prints its length/addr; ps2x_vucap_replay only diffs it byte-for-byte against
our own interpreter). This script is the missing piece: it parses the GIF
packet XGKICK actually sent to the GS -- the vertex data *after* VU1's own
transform, exactly as the game's microprogram computed it -- so a screen Y
value can be read directly, with no matrix multiply, no row/column-convention
guess, and no vertex-stream-offset heuristic.

The GIFtag / PACKED-register decode mirrors GS::processGIFPacket and
GS::writeRegisterPacked in ps2xRuntime/src/lib/ps2_gs_gpu.cpp (register
descriptor table at line ~4911) byte-for-byte -- see that file if this drifts.

Usage:
    python vucap_decode_kick.py capture.vucap                  # summary + first few kicks
    python vucap_decode_kick.py capture.vucap --run 42          # only kicks from run 42
    python vucap_decode_kick.py capture.vucap --limit 0         # all kicks, all vertices
    python vucap_decode_kick.py capture.vucap --csv out.csv     # also write one row per vertex
"""

import argparse
import csv
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from vucap import read_records  # noqa: E402  (reuse the existing container-format parser)

GIF_FMT_PACKED = 0
GIF_FMT_REGLIST = 1
GIF_FMT_IMAGE = 2

REG_NAMES = {
    0x00: "PRIM", 0x01: "RGBAQ", 0x02: "ST", 0x03: "UV", 0x04: "XYZF2",
    0x05: "XYZ2", 0x06: "TEX0_1", 0x07: "TEX0_2", 0x08: "CLAMP_1",
    0x09: "CLAMP_2", 0x0A: "FOG", 0x0C: "XYZF3", 0x0D: "XYZ3", 0x0E: "A+D",
    0x0F: "NOP",
}


class GsState:
    """Mirrors the handful of GS::m_registers fields writeRegisterPacked reads."""

    def __init__(self):
        self.r = self.g = self.b = self.a = 0
        self.q = 1.0
        self.s = self.t = 0.0
        self.u = self.v = 0
        self.fog = 0


def decode_kick_packet(data):
    """Yields dicts, one per XYZ*-kicked vertex, walking `data` as GIFtags."""
    st = GsState()
    offset = 0
    size = len(data)
    while offset + 16 <= size:
        tag_lo, tag_hi = struct.unpack_from("<QQ", data, offset)
        offset += 16

        st.q = 1.0  # processGIFPacket resets RGBAQ.q to 1.0 at every GIFtag
        nloop = tag_lo & 0x7FFF
        flg = (tag_lo >> 58) & 0x3
        nreg = ((tag_lo >> 60) & 0xF) or 16
        pre = bool((tag_lo >> 46) & 1)
        prim_from_tag = (tag_lo >> 47) & 0x7FF

        regs = [(tag_hi >> (i * 4)) & 0xF for i in range(nreg)]

        if flg == GIF_FMT_PACKED:
            for _loop in range(nloop):
                for reg_desc in regs:
                    if offset + 16 > size:
                        return
                    lo, hi = struct.unpack_from("<QQ", data, offset)
                    offset += 16
                    vtx = _apply_packed_register(st, reg_desc, lo, hi)
                    if vtx is not None:
                        if pre:
                            vtx["prim"] = prim_from_tag
                        yield vtx
        elif flg == GIF_FMT_REGLIST:
            # REGLIST carries the same registers but 8 bytes each, no vertex
            # kick side effects worth decoding for this bug (XYZ* only ever
            # observed in PACKED form for VU1 tri-strip output) -- skip payload.
            consumed = nloop * nreg
            offset += consumed * 8 + (8 if (consumed & 1) else 0)
        elif flg == GIF_FMT_IMAGE:
            offset += nloop * 16  # raw pixel payload, not vertex data
        else:
            return  # malformed tag -- stop rather than misparse forward


def _apply_packed_register(st, reg_desc, lo, hi):
    """One case of GS::writeRegisterPacked. Returns a vertex dict or None."""
    if reg_desc == 0x01:  # RGBAQ
        st.r = lo & 0xFF
        st.g = (lo >> 32) & 0xFF
        st.b = hi & 0xFF
        st.a = (hi >> 32) & 0xFF
        return None
    if reg_desc == 0x02:  # ST
        st.s = struct.unpack("<f", struct.pack("<I", lo & 0xFFFFFFFF))[0]
        st.t = struct.unpack("<f", struct.pack("<I", (lo >> 32) & 0xFFFFFFFF))[0]
        q = struct.unpack("<f", struct.pack("<I", hi & 0xFFFFFFFF))[0]
        st.q = q if q != 0.0 else 1.0
        return None
    if reg_desc == 0x03:  # UV
        st.u = lo & 0xFFFF
        st.v = (lo >> 32) & 0xFFFF
        return None
    if reg_desc == 0x0A:  # FOG
        st.fog = (hi >> 36) & 0xFF
        return None
    if reg_desc == 0x0E:  # A+D -- raw GS register write, not a vertex
        return None
    if reg_desc in (0x04, 0x0C):  # XYZF2 / XYZF3: 16-bit x,y, 24-bit z, fog+adc in hi
        adc = ((hi >> 47) & 1) != 0
        return {
            "reg": reg_desc,
            "x": (lo & 0xFFFF) / 16.0,
            "y": ((lo >> 32) & 0xFFFF) / 16.0,
            "z": (hi >> 4) & 0xFFFFFF,
            "r": st.r, "g": st.g, "b": st.b, "a": st.a, "q": st.q,
            "s": st.s, "t": st.t, "u": st.u, "v": st.v,
            "fog": (hi >> 36) & 0xFF if reg_desc == 0x04 else st.fog,
            "adc": adc,
            "kicked": (not adc) if reg_desc == 0x04 else True,
        }
    if reg_desc in (0x05, 0x0D):  # XYZ2 / XYZ3: 16-bit x,y, full 32-bit z
        adc = ((hi >> 47) & 1) != 0
        return {
            "reg": reg_desc,
            "x": (lo & 0xFFFF) / 16.0,
            "y": ((lo >> 32) & 0xFFFF) / 16.0,
            "z": hi & 0xFFFFFFFF,
            "r": st.r, "g": st.g, "b": st.b, "a": st.a, "q": st.q,
            "s": st.s, "t": st.t, "u": st.u, "v": st.v,
            "fog": st.fog,
            "adc": adc,
            "kicked": (not adc) if reg_desc == 0x05 else True,
        }
    return None  # PRIM (0x00) / TEX0 / CLAMP / NOP -- no vertex, safe to ignore


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--run", type=int, default=None, help="only decode KICKDATA from this run index")
    ap.add_argument("--limit", type=int, default=8, help="vertices to print per kick (0 = all)")
    ap.add_argument("--csv", default=None, help="write one row per vertex to this CSV path")
    args = ap.parse_args()

    recs, torn, size = read_records(args.capture)
    if torn:
        print(f"  WARNING: {torn}", file=sys.stderr)

    run_ctx = {}
    for rtype, payload in recs:
        if rtype == 3:  # RUNSTART
            run, tpc, top, itop = struct.unpack_from("<4I", payload)
            run_ctx[run] = {"tpc": tpc, "top": top, "itop": itop}

    csv_writer = None
    csv_file = None
    if args.csv:
        csv_file = open(args.csv, "w", newline="")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["run", "tpc", "top", "kick_addr", "vtx_idx", "reg",
                              "x", "y", "z", "r", "g", "b", "a", "q", "s", "t", "kicked"])

    kick_count = 0
    vtx_total = 0
    y_min = y_max = None
    for rtype, payload in recs:
        if rtype != 6:  # KICKDATA
            continue
        run, addr, sz, _eop = struct.unpack_from("<4I", payload)
        if args.run is not None and run != args.run:
            continue
        packet = payload[16:16 + sz]
        ctx = run_ctx.get(run, {})
        kick_count += 1

        verts = list(decode_kick_packet(packet))
        vtx_total += len(verts)
        shown = verts if args.limit == 0 else verts[:args.limit]

        print(f"kick #{kick_count} run={run} tpc={ctx.get('tpc', -1):#05x} "
              f"top={ctx.get('top', -1):#x} addr={addr:#x} bytes={sz} vertices={len(verts)}")
        for i, v in enumerate(shown):
            print(f"  v{i:3d} reg={REG_NAMES.get(v['reg'], v['reg']):6s} "
                  f"x={v['x']:9.2f} y={v['y']:9.2f} z={v['z']:10d} "
                  f"rgba=({v['r']},{v['g']},{v['b']},{v['a']}) q={v['q']:.3f} "
                  f"st=({v['s']:.3f},{v['t']:.3f}) kicked={v['kicked']}")
        if len(shown) < len(verts):
            print(f"  ... {len(verts) - len(shown)} more (raise --limit to see them)")

        for vtx_idx, v in enumerate(verts):
            if v["kicked"]:
                y_min = v["y"] if y_min is None else min(y_min, v["y"])
                y_max = v["y"] if y_max is None else max(y_max, v["y"])
            if csv_writer:
                csv_writer.writerow([run, ctx.get("tpc", -1), ctx.get("top", -1), addr,
                                      vtx_idx, v["reg"], v["x"], v["y"], v["z"],
                                      v["r"], v["g"], v["b"], v["a"], v["q"], v["s"], v["t"],
                                      v["kicked"]])

    if csv_file:
        csv_file.close()
        print(f"wrote {args.csv}")

    print(f"\n{kick_count} kick(s) decoded, {vtx_total} vertex records", end="")
    if y_min is not None:
        print(f", kicked-vertex Y range [{y_min:.2f} .. {y_max:.2f}]")
    else:
        print()


if __name__ == "__main__":
    main()
