#!/usr/bin/env python3
"""run_pcsx2.py -- Stage B: run conformance.elf on PCSX2 and save its output region.

Boots the ELF on the SDBZ PCSX2 build (-batch -fastboot, same exe and settings as the
sweeps: EE interpreter per the user's ini), polls the header through the DebugServer
(TCP 21512, started by VMManager) until guest main writes DONE, dumps the output region,
then closes PCSX2. No PCSX2 change and no game ISO needed.

    python run_pcsx2.py                 -> out/pcsx2.bin (+ pcsx2.json: status, timing, elf hash)
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from pcsx2_ee import DebugServer, DebugServerError  # noqa: E402

PCSX2_EXE = Path(r"F:\PCSX2-src\bin\pcsx2-qtx64-avx2.exe")


def pcsx2_running():
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq pcsx2-qtx64-avx2.exe"], capture_output=True, text=True)
    return "pcsx2-qtx64-avx2.exe" in r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, default=HERE / "out")
    ap.add_argument("--timeout", type=float, default=300, help="seconds to wait for DONE")
    ap.add_argument("--keep-open", action="store_true", help="leave PCSX2 running (inspect with the pcsx2 MCP)")
    a = ap.parse_args()
    m = json.loads((a.dir / "manifest.json").read_text())
    elf = a.dir / "conformance.elf"
    hdr, ho = m["hdr"], m["hdr_offsets"]
    if pcsx2_running():
        sys.exit("PCSX2 is already running: close it first (this run needs its own instance)")
    env = {k: v for k, v in os.environ.items() if not k.startswith("PS2X_")}   # no PadScript, no captures
    t0 = time.time()
    proc = subprocess.Popen([str(PCSX2_EXE), "-batch", "-fastboot", "-nofullscreen", "--", str(elf.resolve())],
                            cwd=str(a.dir), env=env)
    ds, info, rc = None, {"elf_sha1": hashlib.sha1(elf.read_bytes()).hexdigest()}, 0
    try:
        while ds is None:                                  # the server starts with the VM
            if proc.poll() is not None:
                sys.exit(f"PCSX2 exited (rc={proc.returncode}) before the DebugServer came up")
            if time.time() - t0 > 60:
                sys.exit("no DebugServer on port 21512 after 60 s (is this the sdbz-tools PCSX2 build?)")
            try:
                ds = DebugServer(timeout=10)
            except OSError:
                time.sleep(0.5)
        last_prog, last_change, state = -1, time.time(), "boot"
        while True:
            if proc.poll() is not None:
                sys.exit(f"PCSX2 exited (rc={proc.returncode}) before DONE")
            try:
                h = ds.read_range(hdr, 48)
            except DebugServerError:
                h = bytes(48)
            magic, = struct.unpack_from("<I", h, ho["magic"])
            prog, = struct.unpack_from("<I", h, ho["progress"])
            done, = struct.unpack_from("<I", h, ho["done"])
            if magic == 0x464E4F43 and state == "boot":
                state, last_change = "running", time.time()
                print(f"[pcsx2] ELF loaded after {time.time() - t0:.1f} s")
            if done == m["done_magic"]:
                break
            if prog != last_prog:
                last_prog, last_change = prog, time.time()
            stuck = time.time() - last_change
            if state == "running" and stuck > 20 or time.time() - t0 > a.timeout:
                njobs, = struct.unpack_from("<I", h, ho["njobs"])
                job = max(prog - 1, 0)
                regs = {}
                try:
                    ds.send({"cmd": "pause"})
                    regs = ds.send({"cmd": "read_registers", "cpu": "ee", "category": 0})
                except DebugServerError as e:
                    regs = {"error": str(e)}
                (a.dir / "pcsx2_stuck.json").write_text(json.dumps({"state": state, "progress": prog, "njobs": njobs,
                                                                     "regs": regs}, indent=1))
                tests = sorted(m["tests"], key=lambda t: t["out"])
                per = len(m["vectors"])
                t = tests[min(job // per, len(tests) - 1)] if tests else None
                where = f"job {job}/{njobs} = {t['name']} ({t['text']})" if t and state == "running" else "before the ELF ran"
                print(f"[pcsx2] STUCK {stuck:.0f} s: {where}; registers -> {a.dir / 'pcsx2_stuck.json'}")
                rc = 3
                break
            time.sleep(0.5)
        info.update({"seconds": round(time.time() - t0, 1), "complete": rc == 0, "progress": last_prog})
        status, = struct.unpack_from("<I", ds.read_range(hdr + ho["status"], 4), 0)
        info["cop0_status"] = f"0x{status:08x}"
        data = ds.read_range(m["out_base"], m["out_size"])
        (a.dir / "pcsx2.bin").write_bytes(data)
        (a.dir / "pcsx2.json").write_text(json.dumps(info, indent=1))
        print(f"[pcsx2] {'DONE' if rc == 0 else 'PARTIAL'} in {info['seconds']} s, COP0 Status {info['cop0_status']}, "
              f"{len(data)} bytes -> {a.dir / 'pcsx2.bin'}")
    finally:
        if ds:
            ds.close()
        if not a.keep_open and proc.poll() is None:
            proc.kill()
    sys.exit(rc)


if __name__ == "__main__":
    main()
