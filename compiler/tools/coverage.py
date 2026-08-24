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
        # The probe must be something the compiler genuinely cannot lower.
        # Hardcoding one construct went stale SEVEN times -- `import os`,
        # `class C: pass`, try, lambda, a list comprehension, a generator
        # expression, `yield` -- because the whole point of the project is to
        # implement them. Each time the self-check refused to report, which is
        # the guard working; but a guard that needs hand-editing every few
        # features is a guard that will eventually be edited carelessly.
        #
        # So ask the compiler what it still refuses, rather than asserting it.
        # If it refuses NONE of these, the probe cannot be built and this must
        # fail loudly rather than quietly measure nothing.
        candidates = [
            ("async def",      "async def f():\n    pass\n"),
            ("await",          "async def f():\n    await g()\n"),
            ("match",          "match x:\n    case 1:\n        pass\n"),
            ("raise from",     "try:\n    pass\nexcept E as e:\n    raise V() from e\n"),
            ("posonly params", "def f(a, /, b):\n    pass\n"),
            ("type alias",     "type X = int\n"),
            ("async for",      "async def f():\n    async for i in a:\n        pass\n"),
        ]
        bad = Path(td) / "bad.py"
        chosen = None
        for label, text in candidates:
            bad.write_text(text)
            if run.outcome(str(bad)) != "ok":
                chosen = label
                break
        if chosen is None:
            print("\033[31m\033[1mself-check failed\033[0m")
            print("  No candidate construct is still refused, so no probe can be")
            print("  built and this instrument cannot be shown to detect anything.")
            print("  Add a construct pyc genuinely cannot lower, or retire the check.")
            if args.record:
                args.record.write_text("REFUSED: no probe construct available\n")
            return 1
        print(f"  \033[90mprobe: {chosen} (still refused)\033[0m")
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
