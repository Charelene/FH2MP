#!/usr/bin/env python3
"""Client for the shim's in-process debugger (shim/src/dbg.inc).

  python3 -I shim/fh2dbg.py a                      interactive session on azimuth/a
  python3 -I shim/fh2dbg.py b "bp 8d170b log=rax"  one command
  python3 -I shim/fh2dbg.py a --follow             stream events (breakpoint/watch hits, crash reports)

The instance is a folder name under azimuth/ (reads azimuth/<inst>/data/bin/fh2mp-dbg.port), or give
--port. If a symbol file exists (ghidra/funcs.txt, "<rva-hex> <size-hex> <name>" per line, produced by
tools/ghidra/ExportFuncs.java) every "+0xRVA" in the output is annotated with <function+offset>.
Extra client commands: follow, sym <rva>.
"""
import bisect
import os
import re
import socket
import sys
import time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def load_syms(path):
    rows = []
    try:
        with open(path) as f:
            for ln in f:
                p = ln.split(None, 2)
                if len(p) == 3:
                    rows.append((int(p[0], 16), int(p[1], 16), p[2].strip()))
    except OSError:
        return [], []
    rows.sort()
    return [r[0] for r in rows], rows


class Dbg:
    def __init__(self, port, syms):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""
        self.keys, self.rows = syms

    def sym(self, rva):
        i = bisect.bisect_right(self.keys, rva) - 1
        if i >= 0:
            start, size, name = self.rows[i]
            if rva < start + max(size, 1):
                off = rva - start
                return f"<{name}+{off:#x}>" if off else f"<{name}>"
        return ""

    def annotate(self, text):
        if not self.rows:
            return text
        return re.sub(r"\+0x([0-9a-f]+)\b",
                      lambda m: m.group(0) + (" " + self.sym(int(m.group(1), 16))
                                              if self.sym(int(m.group(1), 16)) else ""), text)

    def cmd(self, line):
        self.s.sendall(line.strip().encode() + b"\n")
        while b"\n.\n" not in self.buf and not self.buf.startswith(b".\n"):
            d = self.s.recv(65536)
            if not d:
                break
            self.buf += d
        if self.buf.startswith(b".\n"):
            out, self.buf = "", self.buf[2:]
        else:
            body, _, self.buf = self.buf.partition(b"\n.\n")
            out = body.decode("latin1") + "\n"
        return self.annotate(out)


def find_port(inst):
    p = os.path.join(ROOT, "azimuth", inst, "data", "bin", "fh2mp-dbg.port")
    with open(p) as f:
        return int(f.read().strip())


def follow(d, since=0):
    try:
        while True:
            out = d.cmd(f"ev {since}")
            for ln in out.splitlines():
                if ln.startswith("# "):
                    since = int(ln[2:])
                elif ln:
                    print(ln, flush=True)
            time.sleep(0.4)
    except KeyboardInterrupt:
        pass


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    port = None
    if "--port" in args:
        i = args.index("--port")
        port = int(args[i + 1])
        del args[i:i + 2]
    syms_path = os.path.join(ROOT, "ghidra", "funcs.txt")
    if "--syms" in args:
        i = args.index("--syms")
        syms_path = args[i + 1]
        del args[i:i + 2]
    inst = args.pop(0)
    d = Dbg(port or find_port(inst), load_syms(syms_path))
    if args and args[0] == "--follow" or (args and args[0] == "follow"):
        follow(d)
        return 0
    if args:
        print(d.cmd(" ".join(args)), end="")
        return 0
    try:
        while True:
            line = input(f"fh2dbg[{inst}]> ").strip()
            if line in ("q", "quit", "exit"):
                break
            if line == "follow":
                follow(d)
            elif line.startswith("sym "):
                print(d.sym(int(line.split()[1], 16)) or "(no symbol)")
            elif line:
                print(d.cmd(line), end="")
    except (EOFError, KeyboardInterrupt):
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
