#!/usr/bin/env python3
"""Bulk EE/IOP memory reader for a running PCSX2, over the DebugServer socket.

Why this exists: the MCP read_memory tool caps at 4096 bytes per call and returns
the hex into the agent's context. Walking a DMA display-list chain needs tens of
KB. This pulls arbitrary ranges straight to disk at no context cost.

Protocol (PCSX2 DebugServer, default TCP 21512): newline-delimited JSON.
    -> {"cmd":"read_memory","cpu":"ee","address":<int>,"length":<int>}
    <- {"ok":true,"hex":"..."}            (or {"ok":false,"error":"..."})
Confirmed against the bundled MCP client at
PCSX2/PCSX2-MCP-v1.0.0-win64/pcsx2-mcp-server/dist/debug-server-client.js.

PCSX2 must be *paused* for a coherent read of a buffer the guest is rewriting.

Usage:
    python pcsx2_ee.py --dump 0x771400 0x4000 out.bin
    python pcsx2_ee.py --peek 0x7717d0 32
"""

import argparse
import json
import socket
import sys

DEFAULT_PORT = 21512
# The server answers much larger requests than the MCP tool exposes, but stay
# modest so a single bad range cannot stall the socket for long.
CHUNK = 16384


class DebugServerError(RuntimeError):
    pass


class DebugServer:
    def __init__(self, host="127.0.0.1", port=DEFAULT_PORT, timeout=15.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def close(self):
        self.sock.close()

    def send(self, cmd):
        self.sock.sendall((json.dumps(cmd) + "\n").encode("utf-8"))
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise DebugServerError("DebugServer closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        resp = json.loads(line.decode("utf-8"))
        if not resp.get("ok", False):
            raise DebugServerError(resp.get("error", "unknown DebugServer error"))
        return resp

    def read_range(self, base, length, cpu="ee"):
        out = bytearray()
        while len(out) < length:
            want = min(CHUNK, length - len(out))
            resp = self.send({"cmd": "read_memory", "cpu": cpu,
                              "address": base + len(out), "length": want})
            got = bytes.fromhex(resp["hex"])
            if not got:
                raise DebugServerError(f"empty read at 0x{base + len(out):08X}")
            out += got
        return bytes(out[:length])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--cpu", default="ee", choices=["ee", "iop"])
    ap.add_argument("--dump", nargs=3, metavar=("ADDR", "LEN", "OUT"),
                    help="read LEN bytes at ADDR into OUT (binary)")
    ap.add_argument("--peek", nargs=2, metavar=("ADDR", "LEN"),
                    help="read and hexdump to stdout")
    ap.add_argument("--vucap", metavar="OUT",
                    help="vucap build only: record VU1/VIF1/XGKICK traffic to OUT (.vucap)")
    ap.add_argument("--frames", type=int, default=60,
                    help="frames to record with --vucap (0 = until --vucap-stop)")
    ap.add_argument("--fullmem", nargs=2, type=int, metavar=("FIRST", "LAST"),
                    help="also store full VU1 data memory for runs FIRST..LAST (1-based)")
    ap.add_argument("--wait", action="store_true",
                    help="with --vucap: poll until the capture is done")
    ap.add_argument("--vucap-status", action="store_true", help="print capture status")
    ap.add_argument("--vucap-stop", action="store_true", help="stop the capture")
    args = ap.parse_args()

    if not (args.dump or args.peek or args.vucap or args.vucap_status or args.vucap_stop):
        ap.error("one of --dump, --peek, --vucap, --vucap-status, --vucap-stop is required")

    srv = DebugServer(args.host, args.port)
    try:
        if args.vucap:
            import os
            import time
            first, last = args.fullmem if args.fullmem else (1, 0)
            srv.send({"cmd": "vucap", "path": os.path.abspath(args.vucap),
                      "frames": args.frames, "fullmem_from": first, "fullmem_to": last})
            print(f"vucap requested -> {os.path.abspath(args.vucap)} ({args.frames} frames)")
            polls = 0
            started = False
            while args.wait:
                st = srv.send({"cmd": "vucap_status"})["data"]
                polls += 1
                print(f"  {st['state']}: frames={st['frames']} runs={st['runs']} "
                      f"kicks={st['kick_chunks']} bytes={st['bytes']}")
                # The request is picked up at the next vsync, so the state seen right
                # after sending it is still the PREVIOUS capture's ("done"/"idle").
                # Only a state reached after armed/capturing belongs to this request.
                if st["state"] in ("armed", "capturing"):
                    started = True
                if (started and st["state"] in ("done", "idle")) or st["state"] == "error" \
                        or (not started and polls > 30):
                    if st["error"]:
                        print(f"  error: {st['error']}")
                    break
                time.sleep(1.0)

        if args.vucap_stop:
            srv.send({"cmd": "vucap_stop"})
            print("vucap stop requested")

        if args.vucap_status:
            print(json.dumps(srv.send({"cmd": "vucap_status"})["data"], indent=2))

        if args.peek:
            addr = int(args.peek[0], 0)
            data = srv.read_range(addr, int(args.peek[1], 0), args.cpu)
            for i in range(0, len(data), 16):
                row = data[i:i + 16]
                hexs = " ".join(f"{b:02x}" for b in row)
                text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
                print(f"{addr + i:08x}  {hexs:<47}  |{text}|")

        if args.dump:
            addr = int(args.dump[0], 0)
            data = srv.read_range(addr, int(args.dump[1], 0), args.cpu)
            with open(args.dump[2], "wb") as fh:
                fh.write(data)
            print(f"wrote {len(data)} bytes from 0x{addr:08X} to {args.dump[2]}")
    finally:
        srv.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
