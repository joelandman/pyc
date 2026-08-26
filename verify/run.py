#!/usr/bin/env python3
"""pyc differential verification harness (agent A5).

  ./verify/run.py --corpus tests/                    # a directory of programs
  ./verify/run.py --libtest                          # CPython Lib/test metric
  ./verify/run.py --corpus tests/ --json out.json    # machine-readable
  ./verify/run.py --corpus tests/ --fail-on P0       # CI gate

The oracle is a real CPython binary. No expected output is stored anywhere in
this tree, by construction (CHARTER I5).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import sys
import sysconfig
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from verify import corpus as corpus_mod  # noqa: E402
from verify.differential import (  # noqa: E402
    Case, CompilerAdapter, DifferentialRunner, Result, Verdict,
)

BOLD, DIM, RED, YEL, GRN, CYA, RST = (
    "\033[1m", "\033[90m", "\033[31m", "\033[33m", "\033[32m", "\033[36m", "\033[0m"
)

_COLOR = {
    Verdict.SILENT_WRONG_ANSWER: RED,
    Verdict.CRASH: YEL,
    Verdict.HANG: YEL,
    Verdict.COMPILE_ERROR: CYA,
    Verdict.STDERR_DIFF: DIM,
    Verdict.MATCH: GRN,
}


def find_pyc() -> Path | None:
    import os
    if (env := os.environ.get("PYC_BINARY")):
        p = Path(env)
        if p.exists():
            return p
    for c in ("build/pyc", "pyc", "build/bin/pyc"):
        p = Path(c)
        if p.exists():
            return p.resolve()
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_argument_group("corpus")
    src.add_argument("--corpus", type=Path, action="append", default=[],
                     help="directory of standalone .py programs (repeatable)")
    src.add_argument("--recursive", action="store_true")
    src.add_argument("--libtest", action="store_true",
                     help="CPython Lib/test/ — the completeness metric (I6)")
    src.add_argument("--libtest-limit", type=int, default=None)
    src.add_argument("--file", type=Path, action="append", default=[],
                     help="a single program (repeatable)")

    cfg = ap.add_argument_group("configuration")
    cfg.add_argument("--pyc", type=Path, default=None, help="the pyc binary")
    cfg.add_argument("--pyc-flag", action="append", default=[],
                     help="extra flag passed to pyc (repeatable)")
    cfg.add_argument("--oracle", type=Path, default=None,
                     help="CPython binary (default: this interpreter)")
    cfg.add_argument("--sysroot", type=Path, default=None,
                     help="read the oracle from a pyc-sysroot.json manifest")
    cfg.add_argument("--stdlib", type=Path, default=None,
                     help="stdlib root for --libtest (default: oracle's own)")
    cfg.add_argument("--jobs", "-j", type=int, default=0, help="0 = cpu_count")
    cfg.add_argument("--run-timeout", type=float, default=30.0)
    cfg.add_argument("--compile-timeout", type=float, default=180.0)
    cfg.add_argument("--no-nondeterminism-probe", action="store_true",
                     help="skip the double-run oracle stability check (faster, "
                          "but nondeterministic cases become false findings)")

    out = ap.add_argument_group("output")
    out.add_argument("--json", type=Path, default=None)
    out.add_argument("--fail-on", choices=["P0", "P1", "P2", "P3", "any", "never"],
                     default="never", help="exit nonzero at or above this priority")
    out.add_argument("--show", type=int, default=8,
                     help="how many findings to show diffs for (0 = none)")
    out.add_argument("--quiet", action="store_true")
    args = ap.parse_args(_normalize_argv(sys.argv[1:]))

    pyc = args.pyc or find_pyc()
    if pyc is None:
        print("error: no pyc binary (use --pyc or set PYC_BINARY)", file=sys.stderr)
        return 2

    oracle = (args.oracle
              or corpus_mod.resolve_sysroot_oracle(args.sysroot)
              or Path(sys.executable))

    cases: list[Case] = []
    for d in args.corpus:
        cases += list(corpus_mod.from_directory(d, recursive=args.recursive))
    for f in args.file:
        cases.append(Case(path=f))
    if args.libtest:
        stdlib = args.stdlib or Path(sysconfig.get_paths()["stdlib"])
        cases += list(corpus_mod.from_cpython_libtest(
            stdlib, oracle, limit=args.libtest_limit))
    if not cases:
        print("error: empty corpus (pass --corpus, --file, or --libtest)",
              file=sys.stderr)
        return 2

    args._resolved_oracle = oracle
    runner = DifferentialRunner(
        oracle=oracle,
        compiler=CompilerAdapter(pyc, tuple(args.pyc_flag)),
        run_timeout=args.run_timeout,
        compile_timeout=args.compile_timeout,
        nondeterminism_probe=not args.no_nondeterminism_probe,
    )

    if not args.quiet:
        print(f"{BOLD}pyc differential verification{RST}")
        print(f"  subject  {pyc}")
        print(f"  oracle   {oracle}")
        print(f"  cases    {len(cases)}\n")

    started = time.time()
    jobs = args.jobs or None
    results: list[Result] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(runner.run, c): c for c in cases}
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            try:
                res = fut.result()
            except Exception as e:  # harness bug, not a compiler finding
                res = Result(futs[fut], Verdict.QUARANTINE_ORACLE_FAILED,
                             detail=f"harness error: {e!r}")
            results.append(res)
            done += 1
            if not args.quiet:
                c = _COLOR.get(res.verdict, DIM)
                print(f"  {done:>4}/{len(cases)}  {c}{res.verdict.name:<28}{RST} "
                      f"{res.case.name}")

    elapsed = time.time() - started
    results.sort(key=lambda r: (r.verdict.value, r.case.name))
    return report(results, args, elapsed)


def _normalize_argv(argv: list[str]) -> list[str]:
    """Let `--pyc-flag -O0` work as well as `--pyc-flag=-O0`.

    argparse treats any value beginning with '-' as another option, so the
    natural spelling fails with "expected one argument". Passing optimization
    flags is the whole point of this option, so rewrite the separated form
    rather than leaving a trap and a footnote.
    """
    out, i = [], 0
    while i < len(argv):
        a = argv[i]
        if a == "--pyc-flag" and i + 1 < len(argv):
            out.append(f"--pyc-flag={argv[i + 1]}")
            i += 2
            continue
        out.append(a)
        i += 1
    return out


def _oracle_identity(args) -> dict:
    """Record which CPython produced this ground truth.

    A baseline is only meaningful relative to its oracle: a different CPython
    can legitimately give different verdicts, which would otherwise read as a
    regression. check_regression.py refuses to compare across a mismatch.
    """
    import subprocess
    exe = getattr(args, "_resolved_oracle", None)
    if exe is None:
        return {}
    try:
        v = subprocess.run([str(exe), "-c",
                            "import sys,platform;"
                            "print('%d.%d.%d' % sys.version_info[:3]);"
                            "print(platform.machine())"],
                           capture_output=True, text=True, timeout=30)
        ver, machine = (v.stdout.strip().splitlines() + ["", ""])[:2]
    except (OSError, subprocess.SubprocessError):
        ver, machine = "", ""
    return {"path": str(exe), "version": ver, "machine": machine}


def report(results: list[Result], args, elapsed: float) -> int:
    counts = Counter(r.verdict for r in results)
    scored = [r for r in results if r.verdict.is_scored]
    matched = counts[Verdict.MATCH]
    total = len(results)
    # THE published number: matched over the whole corpus. The denominator is
    # the corpus, which is fixed, so the number only moves when the compiler
    # does. matched/scored looks more flattering but its denominator flaps with
    # quarantine nondeterminism -- a night with less flakiness scores MORE
    # cases and therefore reports a LOWER rate, which is backwards.
    rate = (100.0 * matched / total) if total else 0.0
    scored_rate = (100.0 * matched / len(scored)) if scored else 0.0

    print(f"\n{BOLD}Summary{RST}  ({elapsed:.1f}s)")
    for v in Verdict:
        if counts[v]:
            c = _COLOR.get(v, DIM)
            tag = f"{v.priority:<3}" if v.is_finding else "   "
            print(f"  {tag} {c}{v.name:<28}{RST} {counts[v]:>5}")

    quarantined = len(results) - len(scored)
    print(f"\n  {BOLD}pass rate  {rate:5.2f}%{RST}  "
          f"({matched}/{total} cases)")
    if quarantined:
        print(f"  {DIM}          {scored_rate:5.2f}%  ({matched}/{len(scored)} "
              f"scored, {quarantined} quarantined — denominator varies, "
              f"not the published number){RST}")

    p0 = counts[Verdict.SILENT_WRONG_ANSWER]
    if p0:
        print(f"\n  {RED}{BOLD}{p0} SILENT WRONG ANSWER(S){RST} — "
              f"pyc exited 0 and produced non-Python results.")
        print(f"  {DIM}CHARTER I1 ranks these above every crash.{RST}")

    if args.show:
        shown = [r for r in results if r.verdict.is_finding][: args.show]
        if shown:
            print(f"\n{BOLD}Findings{RST}")
            for r in shown:
                c = _COLOR.get(r.verdict, DIM)
                print(f"\n  {c}[{r.verdict.priority}] {r.verdict.name}{RST}  "
                      f"{BOLD}{r.case.name}{RST}")
                for line in r.diff_block().splitlines():
                    print(f"    {DIM}{line}{RST}")

    if args.json:
        args.json.write_text(json.dumps({
            "oracle": _oracle_identity(args),
            "subject": {"pyc_flags": list(args.pyc_flag)},
            # Recorded because it CHANGES THE RESULT. Measured on Lib/test:
            # the same binary scored 51 matches at --jobs 4 and 37 at --jobs 12,
            # because unittest prints "Ran N tests in 0.001s" and that line
            # drifts under load. A pass rate quoted without its parallelism is
            # not reproducible.
            "jobs": (args.jobs or 0),
            "corpus": {"libtest": bool(args.libtest),
                       "libtest_limit": args.libtest_limit},
            "pass_rate": rate,            # matched / total corpus -- published
            "scored_rate": scored_rate,   # matched / scored -- informational
            "total_cases": total,
            "scored": len(scored),
            "matched": matched,
            "quarantined": quarantined,
            "elapsed_seconds": elapsed,
            "counts": {v.name: counts[v] for v in Verdict if counts[v]},
            "results": [{
                "case": r.case.name,
                "path": str(r.case.path),
                "verdict": r.verdict.name,
                "priority": r.verdict.priority,
                "detail": r.detail,
            } for r in results],
        }, indent=2) + "\n")
        print(f"\n  wrote {args.json}")

    if args.fail_on == "never":
        return 0
    if args.fail_on == "any":
        return 1 if any(r.verdict.is_finding for r in results) else 0
    threshold = int(args.fail_on[1])
    worst = [r for r in results
             if r.verdict.is_finding and int(r.verdict.priority[1]) <= threshold]
    return 1 if worst else 0


if __name__ == "__main__":
    sys.exit(main())
