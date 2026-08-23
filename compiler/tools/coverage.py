#!/usr/bin/env python3
"""How much of a corpus lowers, and what blocks the rest.

Self-checking by construction (see selfcheck.py): it proves it can tell a
lowerable program from an unsupported one before reporting, and refuses a
result where nearly every input failed identically -- which is what a
misconfigured harness looks like, and what this script did on its first run.

    ./compiler/tools/coverage.py --lower /tmp/pyc_lower verify/corpus/language
"""
from __future__ import annotations

import argparse, collections, os, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import selfcheck as sc

BOLD, DIM, GRN, RST = "\033[1m", "\033[90m", "\033[32m", "\033[0m"
BLOCKER = re.compile(r"\[([^\]]+)\]\s*$")


class Runner:
    def __init__(self, lower: str, python: str, cwd: str):
        self.lower, self.python, self.cwd = lower, python, cwd

    def outcome(self, path: str) -> str:
        """-> 'ok' or a short blocker label."""
        e = subprocess.run([self.python, "-m", "pyc_parse", os.path.abspath(path)],
                           cwd=self.cwd, capture_output=True, text=True)
        if e.returncode:
            return "parse"
        r = subprocess.run([self.lower, "-"], input=e.stdout,
                           capture_output=True, text=True)
        if r.returncode == 0:
            return "ok"
        first = (r.stderr.strip().splitlines() or [""])[0]
        m = BLOCKER.search(first)
        return m.group(1) if m else "?"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lower", required=True)
    ap.add_argument("--python", default="python3")
    ap.add_argument("--parser-cwd", default="compiler")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("corpus", nargs="+", type=Path)
    args = ap.parse_args()

    run = Runner(args.lower, args.python, args.parser_cwd)

    # Prove the instrument works before trusting any number it produces.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        good = Path(td) / "good.py"; good.write_text("x = 1 + 2\n")
        bad = Path(td) / "bad.py";  bad.write_text("import os\n")   # unsupported
        sc.require_detects(
            "coverage probe",
            lambda p: [] if run.outcome(str(p)) == "ok" else [run.outcome(str(p))],
            bad, good)

    files = sc.require_nonempty(
        [p for d in args.corpus for p in sorted(d.rglob("*.py"))], "corpus")

    outcomes = [run.outcome(str(f)) for f in files]
    sc.reject_implausible_uniformity(outcomes, what="corpus files")

    counts = collections.Counter(outcomes)
    ok = counts.pop("ok", 0)
    total = len(outcomes)
    print(f"\n{BOLD}  lowered {ok}/{total} ({100*ok/total:.1f}%){RST}\n")
    if counts:
        print("  blockers:")
        for k, v in counts.most_common(args.top):
            print(f"    {v:4d}  {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
