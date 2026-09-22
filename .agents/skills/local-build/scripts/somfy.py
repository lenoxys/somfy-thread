#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Serial helpers for the somfy-thread console (native USB, 115200 baud).

Usage:
  scripts/somfy.py backup [file]   dump `export` config JSON (default somfy-backup.json)
  scripts/somfy.py watch [secs]    poll `mstat`, print transitions (default 360s)
  scripts/somfy.py cmd <console…>  run one console command, print the reply

Port defaults to /dev/cu.usbmodem14101; override with SOMFY_PORT.
Read-only over serial — flashing lives in flash.sh. No Somfy RF is transmitted.
"""
import os
import sys
import time

import serial

PORT = os.environ.get("SOMFY_PORT", "/dev/cu.usbmodem14101")


def open_port():
    p = serial.Serial(PORT, 115200, timeout=1)
    time.sleep(1.2)
    p.reset_input_buffer()
    return p


def send(p, c, wait=1.2):
    p.write((c + "\r\n").encode())
    p.flush()
    deadline = time.time() + wait
    lines = []
    while time.time() < deadline:
        line = p.readline()
        lines.append(line)
        if line.strip() == b"Done":
            break
    return b"".join(lines).decode(errors="replace")


def first(out, pred):
    for line in out.splitlines():
        s = line.strip()
        if pred(s):
            return s
    return ""


def backup(path="somfy-backup.json"):
    p = open_port()
    out = send(p, "export", 1.5)
    p.close()
    js = first(out, lambda s: s.startswith(("[", "{")) and s.endswith(("]", "}")))
    if not js:
        sys.exit("no JSON returned by `export` — is the board connected and running?")
    with open(path, "w") as f:
        f.write(js)
    print(f"saved {path}: {js[:80]}{'…' if len(js) > 80 else ''}")


def watch(secs=360):
    p = open_port()
    last = None
    t0 = time.time()
    try:
        while time.time() - t0 < secs:
            # require a complete {...} line so a mid-transmission read is skipped
            st = first(send(p, "mstat", 0.8), lambda s: s.startswith("{") and s.endswith("}"))
            if st and st != last:
                print(f"[{int(time.time() - t0):4d}s] {st}", flush=True)
                last = st
            time.sleep(1.5)
    finally:
        p.close()


def cmd(text):
    p = open_port()
    print(send(p, text).strip())
    p.close()


if __name__ == "__main__":
    args = sys.argv[1:] or ["watch"]
    op = args[0]
    if op == "backup":
        backup(*args[1:])
    elif op == "watch":
        watch(int(args[1]) if len(args) > 1 else 360)
    elif op == "cmd" and len(args) > 1:
        cmd(" ".join(args[1:]))
    else:
        sys.exit("usage: somfy.py backup [file] | watch [secs] | cmd <console cmd>")
