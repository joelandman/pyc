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
    ap.add_argument("--record", type=Path, default=None,
                    help="write the measured line to a file, for pasting into "
                         "a commit message verbatim")
    ap.add_argument("corpus", nargs="+", type=Path)
    args = ap.parse_args()

    run = Runner(args.lower, args.python, args.parser_cwd)

    # Invalidate the record BEFORE measuring. If the run refuses or crashes,
    # a stale file must not survive to be pasted into a commit message as if
    # it were this run's result -- which is the same stale-number failure the
    # record exists to prevent.
    if args.record:
        # A refused run leaves an explicit REFUSED marker rather than nothing.
        # An absent file is ambiguous -- it could mean "not run" -- and a stale
        # one is worse. Neither may be mistaken for a measurement.
        args.record.write_text("REFUSED: self-check did not pass\n")

    # Prove the instrument works before trusting any number it produces.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        good = Path(td) / "good.py"; good.write_text("x = 1 + 2\n")
        # Must be something the compiler genuinely cannot lower. A generator
        # expression needs real generators, so it should outlast the others. This probe
        # goes stale by design as coverage grows. `import os`, `class C: pass`, a try statement
        # a lambda and a list
        # comprehension were the first five choices; the self-check refused each
        # time the feature landed, rather than reporting a number from a blind
        # instrument. Update it when the refusal fires -- that is the guard
        # working, not a nuisance.
        bad = Path(td) / "bad.py";  bad.write_text("g = (x for x in y)\n")
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
    line = f"lowered {ok}/{total} ({100*ok/total:.1f}%)"
    print(f"\n{BOLD}  {line}{RST}\n")
    # A number in a commit message must be pasted from a real run, never
    # recalled or estimated. Twice now an estimate went in and had to be
    # amended, so the measurement is made available as text to copy.
    if args.record:
        args.record.write_text(line + "\n")
    if counts:
        print("  blockers:")
        for k, v in counts.most_common(args.top):
            print(f"    {v:4d}  {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
