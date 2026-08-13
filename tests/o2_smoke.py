#!/usr/bin/env python3
"""Thin -O2 smoke: compile two FILE_CASE programs at the user default
opt level and compare stdout to CPython.

The main runner is -O0 only. This exists so an LTO / runtime.bc
regression cannot hide behind a green runner. It is not a substitute
for verifying a feature at both levels.
"""
import os
import shlex
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
    print("ERROR: Could not find pyc binary. Set PYC_BINARY or build first.")
    sys.exit(1)


def run(cmd, timeout=30):
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    return p.stdout, p.returncode


def main():
    pyc = find_pyc()
    tests_dir = os.path.dirname(os.path.abspath(__file__))
    cases = [
        ("hello.py", []),
        ("nbody.py", ["100"]),
    ]
    failed = 0
    for rel, args in cases:
        src = os.path.join(tests_dir, rel)
        qsrc = shlex.quote(src)
        qargs = " ".join(shlex.quote(a) for a in args)
        exp, _ = run(f"python3 {qsrc} {qargs}")
        exp = exp.strip()
        with tempfile.TemporaryDirectory() as td:
            out_bin = os.path.join(td, "t.bin")
            cmd = (
                f"{shlex.quote(pyc)} {qsrc} -o {shlex.quote(out_bin)} -O2 "
                f">/dev/null 2>&1 && {shlex.quote(out_bin)} {qargs}"
            )
            actual, rc = run(cmd, timeout=60)
            actual = actual.strip()
        if rc != 0 or actual != exp:
            failed += 1
            print(f"FAIL {rel} -O2 rc={rc}")
            print("EXP:", repr(exp))
            print("ACT:", repr(actual))
        else:
            print(f"PASS {rel} -O2")
    if failed:
        print(f"o2_smoke: {failed}/{len(cases)} failed")
        sys.exit(1)
    print(f"o2_smoke: {len(cases)}/{len(cases)} passed")
    sys.exit(0)


if __name__ == "__main__":
    main()
