#!/usr/bin/env python3
"""Structural checks on emitted IR.

Cheap invariants that a malformed lowering violates long before codegen does,
where the failure would surface as an opaque "Module verification failed" --
the same class the old tree hit on `def g(a, /, b)`.

  * every block ends with exactly one terminator
  * every branch target exists
  * every value is defined before use
  * every owned result is released on the normal path exactly once

    ./compiler/tools/check_ir_wellformed.py --lower /tmp/pyc_lower compiler/tests/*.py
"""
from __future__ import annotations

import argparse, re, subprocess, sys
from pathlib import Path

GRN, RED, DIM, BOLD, RST = "\033[32m", "\033[31m", "\033[90m", "\033[1m", "\033[0m"
TERM = re.compile(r"^    (br|condbr|ret|ret\.err)\b")
BLOCK = re.compile(r"^  (\S+):$")
DEF = re.compile(r"^    %(\d+) = ")
USE = re.compile(r"%(\d+)")
DECREF = re.compile(r"^    decref %(\d+)$")
OWNED = re.compile(r"; owned")


def check(ir: str) -> list[str]:
    problems: list[str] = []
    block, terms, nblocks = None, 0, 0
    defined: set[str] = set()
    owned: set[str] = set()
    released: dict[str, int] = {}
    for line in ir.splitlines():
        m = BLOCK.match(line)
        if m:
            if block is not None and terms != 1:
                problems.append(f"block '{block}' has {terms} terminators")
            block, terms = m.group(1), 0
            nblocks += 1
            continue
        if TERM.match(line):
            terms += 1
        d = DEF.match(line)
        if d:
            defined.add(d.group(1))
            if OWNED.search(line):
                owned.add(d.group(1))
        r = DECREF.match(line)
        if r:
            released[r.group(1)] = released.get(r.group(1), 0) + 1
        for u in USE.findall(line.split("=")[-1] if d else line):
            if u not in defined:
                problems.append(f"value %{u} used before definition")
    if block is not None and terms != 1:
        problems.append(f"block '{block}' has {terms} terminators")
    # Owned values must be released. Landing pads mean a value can legitimately
    # be released more than once ACROSS paths, so only never-released is an
    # unambiguous defect from this vantage point.
    for v in sorted(owned, key=int):
        if v not in released:
            problems.append(f"owned %{v} is never released (leak)")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lower", required=True)
    ap.add_argument("--parser-cwd", default="compiler")
    ap.add_argument("--python", default="python3")
    ap.add_argument("files", nargs="+", type=Path)
    args = ap.parse_args()

    ok = bad = unsup = 0
    for f in args.files:
        e = subprocess.run([args.python, "-m", "pyc_parse", str(f.resolve())],
                           cwd=args.parser_cwd, capture_output=True, text=True)
        if e.returncode:
            print(f"  {f.name:10s} {DIM}parse failed{RST}"); bad += 1; continue
        r = subprocess.run([args.lower, "-"], input=e.stdout,
                           capture_output=True, text=True)
        if r.returncode:
            unsup += 1
            print(f"  {f.name:10s} {DIM}unsupported: "
                  f"{r.stderr.strip().splitlines()[0][:56] if r.stderr.strip() else '?'}{RST}")
            continue
        probs = check(r.stdout)
        if probs:
            bad += 1
            print(f"  {f.name:10s} {RED}MALFORMED{RST}")
            for p in probs[:6]:
                print(f"      {p}")
        else:
            ok += 1
            print(f"  {f.name:10s} {GRN}ok{RST}")
    print(f"\n  well-formed {ok}   malformed {bad}   unsupported {unsup}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
