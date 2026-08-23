#!/usr/bin/env python3
"""A failing compile must SAY WHY (INTERFACES §1.1).

pyc_parse reports a SyntaxError as a structured envelope on stdout with a
non-zero exit. pycc used to exit on that status alone, discarding the envelope,
so every syntax error surfaced as a bare `exit 1` with nothing printed. Nothing
caught it: the corpus scored those cases as COMPILE_ERROR, which is exactly
what a refusal looks like, so a compiler that failed silently and a compiler
that refused politely were indistinguishable.

This checks the contract itself, and proves it can fail before reporting.
"""
import argparse, re, subprocess, sys, tempfile, os

# (source, expected kind) -- each must produce a diagnostic naming file:line:col
CASES = [
    ("def f(:\n    pass\n",            "SyntaxError"),
    ("    x = 1\n",                    "IndentationError"),
    ("def f():\n\tx=1\n        y=2\n", "TabError"),
    ("x = (1,\n",                      "SyntaxError"),
    ("return 5\n",                     "SyntaxError"),
]

DIAG = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): error: (?P<msg>.+) \[(?P<kind>\w+)\]")


def compile_one(pycc, src, tmpdir, name):
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(src)
    p = subprocess.run([pycc, path, "-o", os.path.join(tmpdir, "out")],
                       capture_output=True, text=True)
    return p.returncode, p.stderr, path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pycc", required=True)
    a = ap.parse_args()

    bad = 0
    with tempfile.TemporaryDirectory() as td:
        for i, (src, kind) in enumerate(CASES):
            rc, err, path = compile_one(a.pycc, src, td, f"d{i}.py")
            first = err.strip().splitlines()[0] if err.strip() else ""
            m = DIAG.match(first)
            if rc == 0:
                print(f"  case {i}: FAIL — compiled successfully, expected a diagnostic")
                bad += 1
            elif not m:
                print(f"  case {i}: FAIL — exit {rc} but no file:line:col diagnostic")
                print(f"           stderr: {err.strip()[:120]!r}")
                bad += 1
            elif m.group("kind") != kind:
                print(f"  case {i}: FAIL — kind {m.group('kind')}, expected {kind}")
                bad += 1
            else:
                print(f"  case {i}: ok — {m.group('kind')} at "
                      f"{m.group('line')}:{m.group('col')}: {m.group('msg')}")

        # The check must be able to PASS as well as fail, or it proves nothing.
        rc, err, _ = compile_one(a.pycc, "print(1)\n", td, "good.py")
        if rc != 0:
            print(f"  self-check: FAIL — a valid program did not compile: {err.strip()[:160]}")
            bad += 1
        else:
            print("  self-check: ok — a valid program compiles with no diagnostic")

    print()
    if bad:
        print(f"\033[31m\033[1m{bad} diagnostic contract violation(s)\033[0m")
        return 1
    print(f"\033[32m\033[1mDIAGNOSED\033[0m — every refused input names file:line:col "
          f"({len(CASES)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
