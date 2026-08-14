#!/usr/bin/env python3
"""Check that uncaught exceptions print File/line frames (I-009 / W3.1).

The main runner compares stdout to CPython. Uncaught exceptions go to
stderr and include a path that differs between the two, so this is a
dedicated harness: compile a known source, run it, and require CPython-
shaped traceback frames.

  Traceback (most recent call last):
    File "<path>", line N, in <name>
  ValueError: ...

pyc currently prints the header and the exception line, but no File/line
frames. This script must fail on that parent until W3.1 lands.
"""
import os
import re
import subprocess
import sys
import tempfile
import textwrap


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


FRAME_RE = re.compile(
    r'^\s*File "([^"]+)", line (\d+), in (\S+)\s*$', re.MULTILINE
)


def compile_and_run(pyc, src):
    src = textwrap.dedent(src)
    with tempfile.TemporaryDirectory() as td:
        py = os.path.join(td, "tb.py")
        bin_path = os.path.join(td, "tb.bin")
        with open(py, "w") as f:
            f.write(src)
        c = subprocess.run(
            [pyc, py, "-o", bin_path, "-O0"],
            capture_output=True, text=True, timeout=30,
        )
        if c.returncode != 0 or not os.path.exists(bin_path):
            return c.returncode, c.stdout, c.stderr, py, "compile"
        r = subprocess.run(
            [bin_path], capture_output=True, text=True, timeout=5,
        )
        return r.returncode, r.stdout, r.stderr, py, "run"


def require_frames(stderr, py_path, want_names, label):
    if "Traceback (most recent call last):" not in stderr:
        print(f"FAIL {label}: missing traceback header")
        print(stderr)
        return False
    frames = FRAME_RE.findall(stderr)
    if not frames:
        print(f"FAIL {label}: no File/line frames")
        print(stderr)
        return False
    base = os.path.basename(py_path)
    names = []
    for path, line, name in frames:
        if os.path.basename(path) != base and path != py_path:
            print(f"FAIL {label}: unexpected file {path!r} (want {base})")
            print(stderr)
            return False
        if int(line) < 1:
            print(f"FAIL {label}: non-positive line {line}")
            print(stderr)
            return False
        names.append(name)
    for n in want_names:
        if n not in names:
            print(f"FAIL {label}: missing frame in {n!r}; have {names}")
            print(stderr)
            return False
    return True


def main():
    pyc = find_pyc()
    cases = [
        (
            "module_raise",
            "raise ValueError('boom')\n",
            ["<module>"],
            "ValueError: boom",
        ),
        (
            "func_raise",
            "def f():\n    raise ValueError('inner')\nf()\n",
            ["<module>", "f"],
            "ValueError: inner",
        ),
        (
            "index_error",
            "print([1][5])\n",
            ["<module>"],
            "IndexError",
        ),
    ]
    failed = 0
    for label, src, names, needle in cases:
        rc, _out, err, py, phase = compile_and_run(pyc, src)
        if phase == "compile":
            print(f"FAIL {label}: compile failed rc={rc}")
            print(err)
            failed += 1
            continue
        if rc == 0:
            print(f"FAIL {label}: expected non-zero exit, got 0")
            failed += 1
            continue
        if needle not in err:
            print(f"FAIL {label}: stderr missing {needle!r}")
            print(err)
            failed += 1
            continue
        if not require_frames(err, py, names, label):
            failed += 1
            continue
        print(f"PASS {label}")
    if failed:
        print(f"check_traceback: {failed}/{len(cases)} failed")
        sys.exit(1)
    print(f"check_traceback: {len(cases)}/{len(cases)} passed")
    sys.exit(0)


if __name__ == "__main__":
    main()
