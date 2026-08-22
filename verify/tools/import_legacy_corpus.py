#!/usr/bin/env python3
"""One-time migration: rescue the old tree's test corpus, drop its expectations.

The old `tests/runner.py` holds 746 inline `(source, expected)` pairs. The
*sources* are a real investment — years of language-coverage work, each one
added because some construct was broken. The *expected* halves are the exact
artifact CHARTER I5 bans, and are the mechanism by which a subset stayed green.

So we take the source halves and throw the expectations away. Ground truth
comes from CPython at run time, forever after.

`tests/*.py` standalone programs are copied across too, minus the harness
scripts (which are runners, not programs).

    ./verify/tools/import_legacy_corpus.py --write

Idempotent. Re-running regenerates the corpus from the legacy tree.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
LEGACY_RUNNER = REPO / "tests" / "runner.py"
LEGACY_TESTS = REPO / "tests"
CORPUS = REPO / "verify" / "corpus"

# Runners and static checkers, not programs under test.
HARNESS = {"runner.py", "o2_smoke.py", "check_dispatch_chain.py",
           "check_gdb.py", "check_speculative_unbox.py", "check_traceback.py"}

HEADER = "# corpus case — ground truth is CPython at run time (CHARTER I5).\n"


def extract_cases(runner: Path) -> tuple[list[str], int]:
    """Pull the source half of every CASES entry. Never the expected half."""
    tree = ast.parse(runner.read_text(encoding="utf-8"))
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if getattr(node.targets[0], "id", "") != "CASES":
            continue
        if not isinstance(node.value, ast.List):
            continue
        out, deferred = [], 0
        for elt in node.value.elts:
            if not isinstance(elt, ast.Tuple) or not elt.elts:
                continue
            src = elt.elts[0]           # [0] is source; [1] is expected — dropped
            if isinstance(src, ast.Constant) and isinstance(src.value, str):
                out.append(src.value)
            else:
                # Non-literal source: these are the FILE_CASES, whose source is
                # `open(__file__/...).read()`. They are not lost -- the files
                # they read are copied wholesale into corpus/programs below.
                deferred += 1
        return out, deferred
    return [], 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="actually write files")
    args = ap.parse_args()

    if not LEGACY_RUNNER.exists():
        print(f"error: {LEGACY_RUNNER} not found", file=sys.stderr)
        return 2

    sources, deferred = extract_cases(LEGACY_RUNNER)
    # Dedupe on normalized content; the legacy list has near-duplicates.
    seen: dict[str, int] = {}
    unique: list[str] = []
    for s in sources:
        body = s.strip("\n")
        if not body.strip():
            continue
        key = hashlib.sha1(body.encode()).hexdigest()
        if key in seen:
            continue
        seen[key] = len(unique)
        unique.append(body)

    programs = [p for p in sorted(LEGACY_TESTS.glob("*.py"))
                if p.name not in HARNESS and not p.name.startswith("check_")]

    print(f"CASES entries        {len(sources) + deferred}")
    print(f"  unique sources     {len(unique)}  ({len(sources)-len(unique)} dupes dropped)")
    print(f"  expectations kept  0   <- the point")
    print(f"  file-backed        {deferred}  -> covered by programs/ below")
    print(f"tests/*.py programs  {len(programs)}  ({len(HARNESS)} harness scripts skipped)")

    if not args.write:
        print("\n(dry run — pass --write)")
        return 0

    lang = CORPUS / "language"
    prog = CORPUS / "programs"
    for d in (lang, prog):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)

    width = len(str(len(unique)))
    for i, body in enumerate(unique, 1):
        (lang / f"case_{i:0{width}d}.py").write_text(HEADER + body + "\n",
                                                     encoding="utf-8")
    for p in programs:
        shutil.copy2(p, prog / p.name)

    print(f"\nwrote {len(unique)} -> {lang.relative_to(REPO)}")
    print(f"wrote {len(programs)} -> {prog.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
