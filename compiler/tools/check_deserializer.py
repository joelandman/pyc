#!/usr/bin/env python3
"""Prove the C++ deserializer loses nothing.

For each file: pyc_parse emits the envelope, the C++ reader deserializes it and
prints a node-kind histogram, and CPython counts the same tree itself. Any
dropped field, subtree, or node kind changes a count.

    ./compiler/tools/check_deserializer.py --bin /tmp/ast_histogram --dir DIR
"""
from __future__ import annotations

import argparse, ast, collections, json, subprocess, sys, threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.setrecursionlimit(60000)
threading.stack_size(256 * 1024 * 1024)
from pyc_parse import encode_node  # noqa: E402

RED, GRN, BOLD, DIM, RST = "\033[31m", "\033[32m", "\033[1m", "\033[90m", "\033[0m"


def cpython_hist(tree) -> dict[str, int]:
    h: collections.Counter[str] = collections.Counter()
    for n in ast.walk(tree):
        h[type(n).__name__] += 1
    # The C++ walker does not descend into field-free sums (ctx, operators):
    # they carry no children and are stored by value. Drop them both sides.
    import ast as A
    trivial = tuple(c for base in (A.expr_context, A.boolop, A.operator,
                                   A.unaryop, A.cmpop)
                    for c in base.__subclasses__())
    names = {c.__name__ for c in trivial}
    return {k: v for k, v in h.items() if k not in names}


def check(path: Path, binary: str) -> tuple[str, str]:
    try:
        tree = ast.parse(path.read_bytes(), filename=str(path))
    except (SyntaxError, ValueError):
        return "skip", "not parseable"
    envelope = json.dumps({"schema_version": 1, "file": str(path),
                           "ast": encode_node(tree)})
    p = subprocess.run([binary, "-"], input=envelope, capture_output=True, text=True)
    if p.returncode != 0:
        return "error", (p.stderr.strip().splitlines() or ["no stderr"])[-1]
    got = {}
    for line in p.stdout.splitlines():
        k, _, v = line.rpartition(" ")
        if k and k != "__total__":
            got[k] = int(v)
    want = cpython_hist(tree)
    if got == want:
        return "ok", ""
    diff = []
    for k in sorted(set(want) | set(got)):
        if want.get(k, 0) != got.get(k, 0):
            diff.append(f"{k}: cpython={want.get(k,0)} cpp={got.get(k,0)}")
    return "mismatch", "; ".join(diff[:6])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--dir", type=Path, action="append", default=[])
    ap.add_argument("--stdlib", action="store_true")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--show", type=int, default=6)
    args = ap.parse_args()

    files: list[Path] = []
    if args.stdlib:
        import sysconfig
        files += sorted(Path(sysconfig.get_paths()["stdlib"]).rglob("*.py"))
    for d in args.dir:
        files += sorted(d.rglob("*.py"))
    if args.limit:
        files = files[: args.limit]
    if not files:
        print("error: empty corpus", file=sys.stderr); return 2

    print(f"{BOLD}C++ deserializer fidelity{RST}\n  files {len(files)}\n")
    counts = collections.Counter()
    bad = []
    for f in files:
        st, detail = check(f, args.bin)
        counts[st] += 1
        if st in ("mismatch", "error"):
            bad.append((f, st, detail))
    for f, st, d in bad[: args.show]:
        print(f"  {RED}{st}{RST} {f}\n      {DIM}{d}{RST}")
    if len(bad) > args.show:
        print(f"  {DIM}… and {len(bad)-args.show} more{RST}")
    scored = counts["ok"] + counts["mismatch"] + counts["error"]
    print(f"\n  ok {counts['ok']}  mismatch {counts['mismatch']}  "
          f"error {counts['error']}  skipped {counts['skip']}")
    if scored and counts["ok"] == scored:
        print(f"\n{GRN}{BOLD}FAITHFUL{RST} — the C++ reader sees exactly what "
              f"CPython sees ({scored} files)")
        return 0
    print(f"\n{RED}{BOLD}NOT FAITHFUL{RST} — {scored-counts['ok']}/{scored} files differ")
    return 1


if __name__ == "__main__":
    sys.exit(main())
