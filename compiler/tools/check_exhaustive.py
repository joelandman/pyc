#!/usr/bin/env python3
"""Prove I4 is enforced, by breaking it on purpose.

CHARTER I4 says adding an AST node kind must BREAK THE BUILD. In C++20 that
holds because a std::visit over an overload set missing an alternative is a
hard compile error -- but only if every visitor really is exhaustive and no
generic arm has crept in. lint_visitors.py checks the second half statically;
this checks the first half empirically.

For each visitor arm in a source file: delete it, compile, and require
failure. An arm whose removal still compiles is not participating in the
guarantee.

    ./compiler/tools/check_exhaustive.py compiler/src/lower.cpp -- -Icompiler/include
"""
from __future__ import annotations

import argparse, re, subprocess, sys, tempfile
from pathlib import Path

ARM = re.compile(r"^\s*\[&\]\(const (\w+)&")
GRN, RED, DIM, BOLD, RST = "\033[32m", "\033[31m", "\033[90m", "\033[1m", "\033[0m"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("--limit", type=int, default=0, help="0 = every arm")
    ap.add_argument("cxxflags", nargs="*", default=[])
    args = ap.parse_args()

    text = args.source.read_text()
    lines = text.splitlines(keepends=True)
    arms = [(i, ARM.match(l).group(1)) for i, l in enumerate(lines) if ARM.match(l)]
    if not arms:
        print(f"no visitor arms found in {args.source}", file=sys.stderr)
        return 2
    if args.limit:
        arms = arms[: args.limit]

    print(f"{BOLD}I4 exhaustiveness{RST}  {args.source}  ({len(arms)} arms)\n")
    flags = [a for a in args.cxxflags if a != "--"]
    weak = []
    with tempfile.TemporaryDirectory() as td:
        probe = Path(td) / "probe.cpp"
        for idx, name in arms:
            probe.write_text("".join(lines[:idx] + lines[idx + 1:]))
            p = subprocess.run(["g++", "-std=c++20", "-fsyntax-only",
                                *flags, str(probe)],
                               capture_output=True, text=True)
            if p.returncode == 0:
                weak.append(name)
                print(f"  {RED}WEAK{RST}  removing the {name} arm still compiles")

    if weak:
        print(f"\n{RED}{BOLD}{len(weak)} arm(s) do not participate in I4{RST}")
        print("  A visitor that still compiles with an arm missing will also "
              "compile\n  when CPython adds a node kind — silently not "
              "handling it.")
        return 1
    print(f"{GRN}{BOLD}ENFORCED{RST} — every arm's removal breaks the build "
          f"({len(arms)} checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
