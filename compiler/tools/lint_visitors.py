#!/usr/bin/env python3
"""Enforce the generic-arm ban (INTERFACES.md §2.3, CHARTER I4).

A std::visit over an overload set that omits an alternative is a hard compile
error -- that is what gives I4 its teeth in C++20. A generic `auto` arm defeats
it: the code compiles and silently swallows every node kind CPython adds later.
That is the single hole in the guarantee, so it is closed here.

    ./compiler/tools/lint_visitors.py compiler/
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# A lambda arm whose parameter type is deduced: [](auto ...), [](const auto ...)
GENERIC_ARM = re.compile(r"\[\s*\]\s*\(\s*(?:const\s+)?auto\b")
# Opt out for a genuinely generic helper, which must say why.
ALLOW = re.compile(r"//\s*lint:\s*allow-generic-arm\b")


def scan(path: Path) -> list[tuple[int, str]]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    # Only files that actually visit our sum types are in scope.
    text = "\n".join(lines)
    if "pyc/ast/" not in text and "pyc/ir/" not in text and "pyc::ast" not in text:
        return []
    out = []
    for i, line in enumerate(lines, 1):
        if GENERIC_ARM.search(line) and not ALLOW.search(line):
            out.append((i, line.strip()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+", type=Path)
    args = ap.parse_args()

    files: list[Path] = []
    for p in args.paths:
        files += sorted(p.rglob("*.cpp")) + sorted(p.rglob("*.hpp")) if p.is_dir() else [p]

    bad = 0
    for f in files:
        for lineno, text in scan(f):
            bad += 1
            print(f"{f}:{lineno}: generic arm in an AST/IR visitor\n"
                  f"    {text}")
    if bad:
        print(f"\n{bad} generic arm(s). A generic arm silently swallows every "
              f"node kind\nCPython adds in future — handle each case, and emit "
              f"a diagnostic for the\nrest (CHARTER I1/I4). If a helper really "
              f"is generic, mark it:\n    // lint: allow-generic-arm <reason>")
        return 1
    print(f"ok — no generic arms in {len(files)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
