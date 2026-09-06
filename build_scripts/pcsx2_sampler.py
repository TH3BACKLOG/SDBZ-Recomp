#!/usr/bin/env python3
"""Time-series sampler for a *running* PCSX2, over the Pine IPC socket.

Why this exists: the two existing paths both answer a different question.
  * build_scripts/pcsx2_ee.py talks to the DebugServer (21512) and wants the
    emulator PAUSED -- it gets one coherent snapshot of a big range.
  * The pcsx2 MCP tools are one request/response per agent turn, so a few
    hundred samples costs a few hundred round-trips through the agent.

This one polls a handful of EE words at a fixed rate while the game RUNS and
writes a CSV, so "is this counter rising or frozen?" becomes a measurement
instead of an inference.

Protocol (Pine, default TCP 28011), cross-checked against PCSX2's PINE.cpp via
PCSX2/PCSX2-MCP-v1.0.0-win64/pcsx2-mcp-server/dist/pine-client.js:
    request : [u32 LE total size incl. itself] [cmd byte] [args...] ...
    reply   : [u32 LE total size incl. itself] [u8 result 0=OK/0xFF=FAIL] [payload...]
Several commands may share one packet; the reply carries ONE result byte and
then each command's payload concatenated in order.  That batching claim is an
assumption about PINE.cpp, so it is CHECKED at connect time -- see
check_batching(), which falls back to one command per packet if the check fails.

Enable Pine in PCSX2: Settings -> Advanced -> Enable Pine, slot 28011.

Usage:
    python pcsx2_sampler.py --verify --preset sofdec
    python pcsx2_sampler.py --preset sofdec --hz 10 --duration 60 --out sofdec.csv
    python pcsx2_sampler.py --addr w6tick=0x441960 --addr park=0x441924:32 --hz 20
"""

import argparse
import csv
import os
import socket
import struct
import sys
import time

DEFAULT_PORT = 28011

MSG_READ = {8: 0, 16: 1, 32: 2, 64: 3}
MSG_STATUS = 15
MSG_TITLE = 11
MSG_ID = 12

STATUS_NAME = {0: "Running", 1: "Paused", 2: "Shutdown"}

# Field tables live in build_scripts/presets.py so this sampler, the runtime's
# PS2X_TRACE_WATCH sampler, and differential.py all read ONE definition. Column
# names are the join key for the diff, so they must not drift apart.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from presets import PRESETS  # noqa: E402



class PineError(RuntimeError):
    pass


class Pine:
    def __init__(self, host="127.0.0.1", port=DEFAULT_PORT, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.batch = True

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _recv_exactly(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise PineError("Pine closed the connection mid-reply")
            buf += chunk
        return buf

    def _exchange(self, payload):
        """Send one packet, return the reply payload starting at the result byte."""
        self.sock.sendall(struct.pack("<I", len(payload) + 4) + payload)
        size = struct.unpack("<I", self._recv_exactly(4))[0]
        if size < 5 or size > 650000:
            raise PineError("invalid reply size %d" % size)
        body = self._recv_exactly(size - 4)
        if body[0] != 0:
            raise PineError("Pine error code 0x%02X" % body[0])
        return body

    # -- single commands -------------------------------------------------
    def _read_cmd(self, addr, width):
        return struct.pack("<BI", MSG_READ[width], addr)

    def read(self, addr, width=32):
        return _decode(self._exchange(self._read_cmd(addr, width)), 1, width)[0]

    def status(self):
        return struct.unpack("<I", self._exchange(bytes([MSG_STATUS]))[1:5])[0]

    def _string(self, cmd):
        body = self._exchange(bytes([cmd]))
        if len(body) < 5:
            return ""
        n = struct.unpack("<I", body[1:5])[0]
        if n <= 0 or len(body) < 5 + n:
            return ""
        return body[5:5 + n].decode("utf-8", "replace").rstrip("\0")

    def title(self):
        return self._string(MSG_TITLE)

    def game_id(self):
        return self._string(MSG_ID)

    # -- batched sample --------------------------------------------------
    def check_batching(self, probe_addr=0x100000):
        """Verify the one-packet-many-commands assumption before trusting it.

        Batches three reads of the SAME address; a correct reply is exactly
        1 + 3*4 bytes with three identical values.  Anything else -> no batching.
        """
        try:
            body = self._exchange(self._read_cmd(probe_addr, 32) * 3)
            vals, used = _decode(body, 1, 32, count=3)
            self.batch = used == len(body) and len(set(vals)) == 1
        except (PineError, OSError, struct.error):
            self.batch = False
        return self.batch

    # A word of guest CODE. Pine answers an unreadable address with 0 and no
    # error, so a dead read path is shaped exactly like a zeroed global -- and
    # "every watched field is 0" cannot tell the two apart, because at boot
    # every watched field really IS 0. Reading one address that must be
    # nonzero whenever EE memory is mapped settles it in the same packet.
    SENTINEL = 0x100008

    def sample(self, fields):
        """fields: [(name, addr, width)] -> (status, sentinel, [values]).

        Status rides in the same packet as the reads, so a paused frame cannot
        be mistaken for a frozen counter; the sentinel rides along for the
        same reason, so a dead read cannot be mistaken for a cleared field.
        """
        if self.batch:
            payload = (bytes([MSG_STATUS])
                       + self._read_cmd(self.SENTINEL, 32)
                       + b"".join(self._read_cmd(a, w) for _, a, w in fields))
            body = self._exchange(payload)
            st = struct.unpack("<I", body[1:5])[0]
            off = 5
            sentinel, off = _decode(body, off, 32)
            vals = []
            for _, _, w in fields:
                v, off = _decode(body, off, w)
                vals.append(v)
            return st, sentinel, vals
        return (self.status(), self.read(self.SENTINEL, 32),
                [self.read(a, w) for _, a, w in fields])


def _decode(body, off, width, count=1):
    fmt = {8: "<B", 16: "<H", 32: "<I", 64: "<Q"}[width]
    size = width // 8
    out = []
    for _ in range(count):
        if len(body) < off + size:
            raise PineError("reply shorter than the commands sent")
        out.append(struct.unpack(fmt, body[off:off + size])[0])
        off += size
    return (out, off) if count > 1 else (out[0], off)


def parse_addr(spec):
    if "=" not in spec:
        raise argparse.ArgumentTypeError("expected NAME=0xADDR[:WIDTH], got %r" % spec)
    name, rhs = spec.split("=", 1)
    width = 32
    if ":" in rhs:
        rhs, w = rhs.rsplit(":", 1)
        if int(w) not in MSG_READ:
            raise argparse.ArgumentTypeError("width must be 8/16/32/64, got %s" % w)
        width = int(w)
    return name.strip(), int(rhs, 0), width


def summarize(fields, rows, elapsed):
    """Per-field verdict.  Transitions, not sample count, answer rising vs frozen."""
    print("\n--- %d samples over %.2fs ---" % (len(rows), elapsed))
    statuses = {r["status"] for r in rows}
    if statuses - {"Running"}:
        print("  !! emulator was not Running for part of the window: %s" % sorted(statuses))
        print("     a frozen counter here proves nothing -- rerun while it runs")

    # Verdicts use readable rows only. A dead-read row reports 0 for every
    # field, which reads as a transition into zero and back out again -- fake
    # motion, and worse, fake non-monotonicity that disarms rate comparison.
    captured, all_rows = len(rows), rows
    dropped = [r for r in rows if int(r.get("unreadable", 0) or 0)]
    rows = [r for r in rows if not int(r.get("unreadable", 0) or 0)]
    if dropped:
        print("  !! %d of %d samples unreadable (paused, or sentinel read 0)"
              % (len(dropped), len(dropped) + len(rows)))
        print("     they stay in the CSV, flagged, and are excluded here")
    if len(rows) < 2:
        print("  !! nothing readable in this window -- no verdicts")
        return

    # all_rows, not rows: a gap left by a dropped sample is not a slow loop.
    intervals = sorted(all_rows[i]["t_ms"] - all_rows[i - 1]["t_ms"]
                       for i in range(1, len(all_rows)))
    if intervals:
        p50 = intervals[len(intervals) // 2]
        p95 = intervals[min(len(intervals) - 1, int(len(intervals) * 0.95))]
        # captured, not len(rows): this line answers "did the sampler keep up",
        # which is about the loop, not about how many samples survived filtering.
        print("  achieved rate %.2f Hz  (interval p50 %.1fms, p95 %.1fms)"
              % (captured / elapsed, p50, p95))

    pad = max(len(n) for n, _, _ in fields)
    for name, addr, w in fields:
        seq = [r[name] for r in rows]
        trans = sum(1 for i in range(1, len(seq)) if seq[i] != seq[i - 1])
        verdict = "FROZEN" if trans == 0 else "CHANGING %.2f/s" % (trans / elapsed if elapsed else 0.0)
        mono = ""
        if trans and all(seq[i] >= seq[i - 1] for i in range(1, len(seq))):
            mono = " monotonic-up"
        print("  %-*s @0x%08X:%-2d  %s%s  first=0x%X last=0x%X distinct=%d"
              % (pad, name, addr, w, verdict, mono, seq[0], seq[-1], len(set(seq))))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--addr", type=parse_addr, action="append", default=[],
                    metavar="NAME=0xADDR[:WIDTH]")
    ap.add_argument("--preset", choices=sorted(PRESETS))
    ap.add_argument("--hz", type=float, default=10.0)
    ap.add_argument("--duration", type=float, default=30.0,
                    help="seconds; 0 = until Ctrl-C")
    ap.add_argument("--out", help="CSV path (default: summary only)")
    ap.add_argument("--verify", action="store_true",
                    help="connect, print game/status, check batching, read once, exit")
    ap.add_argument("--no-batch", action="store_true",
                    help="one command per packet (slower, no batching assumption)")
    args = ap.parse_args()

    fields = list(PRESETS.get(args.preset, [])) + args.addr
    if not fields and not args.verify:
        ap.error("give --preset and/or --addr (or --verify)")

    try:
        pine = Pine(args.host, args.port)
    except OSError as e:
        print("cannot reach Pine at %s:%d -- is PCSX2 running with "
              "Settings > Advanced > Enable Pine? (%s)" % (args.host, args.port, e),
              file=sys.stderr)
        return 2

    try:
        st = pine.status()
        print("connected: %s [%s]  status=%s"
              % (pine.title() or "(no title)", pine.game_id() or "?",
                 STATUS_NAME.get(st, st)))
        if args.no_batch:
            pine.batch = False
            print("batched reads: disabled by --no-batch")
        else:
            print("batched reads: %s" % ("yes" if pine.check_batching()
                                         else "no (one packet per read)"))
        if args.verify:
            for name, addr, w in fields:
                print("  %s @0x%08X:%d = 0x%X" % (name, addr, w, pine.read(addr, w)))
            return 0
        if st != 0:
            print("  !! status is %s, not Running -- start the game before sampling"
                  % STATUS_NAME.get(st, st), file=sys.stderr)

        period = 1.0 / args.hz if args.hz > 0 else 0.0
        deadline = time.monotonic() + args.duration if args.duration > 0 else None
        rows = []
        t0 = time.monotonic()
        print("sampling %d field(s) at %gHz -- Ctrl-C to stop early"
              % (len(fields), args.hz))
        try:
            nxt = t0
            while deadline is None or time.monotonic() < deadline:
                now = time.monotonic()
                if now < nxt:
                    time.sleep(nxt - now)
                nxt += period
                s, sentinel, vals = pine.sample(fields)
                name = STATUS_NAME.get(s, s)
                # Only the sentinel decides this. The first version of this
                # check used "every watched field reads 0", which flagged 160
                # perfectly good pre-init samples in the 09-05 boot capture --
                # before the movie opens those fields ARE all zero. An
                # all-zero sample is data; a zero sentinel is a dead read.
                dead = sentinel == 0
                row = {"t_ms": (time.monotonic() - t0) * 1000.0,
                       "status": name,
                       "unreadable": 1 if (dead or name != "Running") else 0,
                       "sentinel": sentinel}
                row.update({n: v for (n, _, _), v in zip(fields, vals)})
                rows.append(row)
        except KeyboardInterrupt:
            print("\ninterrupted -- keeping what was sampled")
        elapsed = time.monotonic() - t0

        if not rows:
            print("no samples taken", file=sys.stderr)
            return 1

        if args.out:
            parent = os.path.dirname(os.path.abspath(args.out))
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(args.out, "w", newline="", encoding="utf-8") as fh:
                wr = csv.DictWriter(fh,
                                    fieldnames=["t_ms", "status", "unreadable",
                                                "sentinel"]
                                    + [n for n, _, _ in fields])
                wr.writeheader()
                for r in rows:
                    wr.writerow(dict(r, t_ms="%.1f" % r["t_ms"]))
            print("wrote %s" % args.out)

        summarize(fields, rows, elapsed)
        return 0
    except (PineError, OSError) as e:
        print("pine: %s" % e, file=sys.stderr)
        return 2
    finally:
        pine.close()


if __name__ == "__main__":
    sys.exit(main())
