#!/usr/bin/env python3
"""A1 totality proof.

For every .py file in a corpus: parse -> encode -> decode -> compare
ast.dump(include_attributes=True). Any dropped field, lost attribute, or
mangled constant shows up as a mismatch.

This is the property the old tree could never have satisfied: its ASTNode
flattened FunctionDef's body, defaults and decorators into one children vector
and never read posonlyargs, kwonlyargs or annotations at all.

    ./compiler/tools/roundtrip.py --stdlib          # CPython's Lib/
    ./compiler/tools/roundtrip.py --dir some/pkg
"""

from __future__ import annotations

import argparse
import ast
import concurrent.futures
import json
import sys
import sysconfig
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pyc_parse import decode_node, encode_node  # noqa: E402

RED, GRN, YEL, DIM, BOLD, RST = (
    "\033[31m", "\033[32m", "\033[33m", "\033[90m", "\033[1m", "\033[0m")


def check(path: Path) -> tuple[str, str]:
    """-> (status, detail). status in {ok, mismatch, error, skip}."""
    try:
        src = path.read_bytes()
    except OSError as e:
        return "skip", f"unreadable: {e}"
    try:
        tree = ast.parse(src, filename=str(path))
    except (SyntaxError, ValueError):
        # Lib/test contains files that are deliberately invalid syntax for the
        # current version. Not our failure; excluded from the denominator.
        return "skip", "not parseable by this interpreter"

    try:
        blob = json.dumps(encode_node(tree))       # must survive real JSON,
        back = decode_node(json.loads(blob))       # not just the dict form
    except Exception:
        return "error", traceback.format_exc(limit=3).strip().splitlines()[-1]

    want = ast.dump(tree, include_attributes=True)
    got = ast.dump(back, include_attributes=True)
    if want == got:
        return "ok", ""
    # Report the first divergence, with context, rather than two huge dumps.
    for i, (a, b) in enumerate(zip(want, got)):
        if a != b:
            return "mismatch", f"at char {i}:\n      want …{want[max(0,i-60):i+60]}…\n      got  …{got[max(0,i-60):i+60]}…"
    return "mismatch", f"length differs: want {len(want)} got {len(got)}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stdlib", action="store_true", help="CPython's whole Lib/")
    ap.add_argument("--dir", type=Path, action="append", default=[])
    ap.add_argument("--jobs", "-j", type=int, default=0)
    ap.add_argument("--show", type=int, default=5)
    args = ap.parse_args()

    files: list[Path] = []
    if args.stdlib:
        files += sorted(Path(sysconfig.get_paths()["stdlib"]).rglob("*.py"))
    for d in args.dir:
        files += sorted(d.rglob("*.py"))
    if not files:
        print("error: empty corpus (--stdlib or --dir)", file=sys.stderr)
        return 2

    print(f"{BOLD}A1 AST round-trip{RST}")
    print(f"  interpreter  {sys.version.split()[0]}")
    print(f"  files        {len(files)}\n")

    counts = {"ok": 0, "mismatch": 0, "error": 0, "skip": 0}
    bad: list[tuple[Path, str, str]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs or None) as ex:
        for path, (status, detail) in zip(files, ex.map(check, files)):
            counts[status] += 1
            if status in ("mismatch", "error"):
                bad.append((path, status, detail))

    scored = counts["ok"] + counts["mismatch"] + counts["error"]
    for path, status, detail in bad[: args.show]:
        c = RED if status == "mismatch" else YEL
        print(f"  {c}{status}{RST} {path}\n      {DIM}{detail}{RST}")
    if len(bad) > args.show:
        print(f"  {DIM}… and {len(bad) - args.show} more{RST}")

    print(f"\n  ok {counts['ok']}  mismatch {counts['mismatch']}  "
          f"error {counts['error']}  skipped {counts['skip']}")
    if scored and counts["ok"] == scored:
        print(f"\n{GRN}{BOLD}TOTAL{RST} — every node, field and attribute "
              f"survived the round-trip ({scored} files)")
        return 0
    print(f"\n{RED}{BOLD}NOT TOTAL{RST} — {scored - counts['ok']}/{scored} files lost information")
    return 1


if __name__ == "__main__":
    sys.exit(main())
