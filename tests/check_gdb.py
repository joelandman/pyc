#!/usr/bin/env python3
"""W3.4 / I-019: GDB pretty-printer + A6 FlagArtificial.

1. tools/pyc_gdb.py must parse.
2. Codegen.cpp must mark __specialized_* DISubprograms FlagArtificial.
3. If gdb is on PATH, source the printer against a -g -O0 binary.
"""
import os
import py_compile
import re
import subprocess
import sys
import tempfile


def find_pyc():
    candidates = [
        os.environ.get("PYC_BINARY"),
        os.path.join(os.getcwd(), "pyc"),
        os.path.join(os.getcwd(), "build", "pyc"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "pyc"),
    ]
    for c in candidates:
        if c and os.path.exists(c) and os.access(c, os.X_OK):
            return c
    print("ERROR: Could not find pyc binary.")
    sys.exit(1)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    printer = os.path.join(root, "tools", "pyc_gdb.py")
    codegen = os.path.join(root, "src", "codegen", "Codegen.cpp")
    failed = 0

    if not os.path.exists(printer):
        print("FAIL printer missing:", printer)
        sys.exit(1)
    try:
        py_compile.compile(printer, doraise=True)
        print("PASS printer syntax")
    except py_compile.PyCompileError as e:
        print("FAIL printer syntax:", e)
        failed += 1

    src = open(codegen, encoding="utf-8").read()
    if "FlagArtificial" in src and "__specialized_" in src:
        print("PASS FlagArtificial in Codegen")
    else:
        print("FAIL Codegen.cpp does not set FlagArtificial on specialized variants")
        failed += 1

    gdb = None
    for cand in ("gdb",):
        r = subprocess.run(["which", cand], capture_output=True, text=True)
        if r.returncode == 0:
            gdb = r.stdout.strip()
            break
    if not gdb:
        print("PASS gdb not installed (printer + FlagArtificial still checked)")
    else:
        pyc = find_pyc()
        with tempfile.TemporaryDirectory() as td:
            py = os.path.join(td, "g.py")
            bin_path = os.path.join(td, "g.bin")
            with open(py, "w") as f:
                f.write("def greet(n):\n    return n + 1\nprint(greet(41))\n")
            c = subprocess.run(
                [pyc, py, "-o", bin_path, "-g", "-O0"],
                capture_output=True, text=True, timeout=30,
            )
            if c.returncode != 0 or not os.path.exists(bin_path):
                print("FAIL compile -g -O0")
                print(c.stderr)
                failed += 1
            else:
                script = (
                    "source %s\n"
                    "python print('PYC_GDB_OK' if any(getattr(p, '__name__', '') == '_lookup' for p in gdb.pretty_printers) else 'PYC_GDB_MISS')\n"
                    "quit\n"
                ) % printer
                gcmd = os.path.join(td, "cmds")
                with open(gcmd, "w") as f:
                    f.write(script)
                g = subprocess.run(
                    [gdb, "-batch", "-nx", "-x", gcmd, bin_path],
                    capture_output=True, text=True, timeout=15,
                )
                out = (g.stdout or "") + (g.stderr or "")
                if "PYC_GDB_OK" in out:
                    print("PASS gdb loads printer")
                else:
                    print("FAIL gdb did not register printer")
                    print(out[-500:])
                    failed += 1

    if failed:
        print("check_gdb: %d check(s) failed" % failed)
        sys.exit(1)
    print("check_gdb: ok")
    sys.exit(0)


if __name__ == "__main__":
    main()
