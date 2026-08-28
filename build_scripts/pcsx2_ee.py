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
    args = ap.parse_args()

    if not args.dump and not args.peek:
        ap.error("one of --dump or --peek is required")

    srv = DebugServer(args.host, args.port)
    try:
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
