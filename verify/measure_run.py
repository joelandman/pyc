#!/usr/bin/env python3
"""Run the measurement over a corpus and write the record.

  ./verify/measure_run.py --corpus verify/corpus/language --json out.json
  ./verify/measure_run.py --libtest --jobs 4

Reports measurements. The only derived number is the pass rate, which is the
fraction of cases with no IMPACTFUL flag -- one collective figure over facts,
not a severity model.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import sys
import platform
import sysconfig
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from verify import corpus as corpus_mod            # noqa: E402
from verify.measure import (                       # noqa: E402
    SCHEMA, Case, Compiler, Flag, Measurement, Measurer,
)

BOLD, DIM, RED, YEL, GRN, RST = (
    "\033[1m", "\033[90m", "\033[31m", "\033[33m", "\033[32m", "\033[0m")

COLOUR = {
    Flag.STDOUT_DIFFERS: RED, Flag.EXIT_DIFFERS: RED,
    Flag.ORACLE_UNSTABLE: YEL, Flag.TIMEOUT: YEL,
    Flag.DID_NOT_COMPILE: DIM, Flag.STDERR_DIFFERS: DIM,
}


def find_pyc() -> Path | None:
    import os
    if (env := os.environ.get("PYC_BINARY")):
        if Path(env).exists():
            return Path(env)
    for c in ("compiler/tools/pycc", "build/pyc", "pyc"):
        if Path(c).exists():
            return Path(c).resolve()
    return None


def oracle_identity(oracle: Path) -> dict:
    """What the comparison needs to know about the oracle.

    `version` is X.Y.Z only, and `banner` is the full -VV line. The split is
    deliberate: a comparison must refuse across CPython versions, but CI builds
    its own sysroot, so the banner's build timestamp differs from a local one
    for the very same interpreter. Gating on the banner would make every CI run
    "not comparable" forever.
    """
    import subprocess
    ident = {"path": str(oracle), "version": "", "banner": "",
             "machine": platform.machine()}
    try:
        r = subprocess.run(
            [str(oracle), "-c",
             "import sys,platform;print(platform.python_version());"
             "print(sys.version.replace(chr(10),' '))"],
            capture_output=True, text=True, timeout=30)
        if r.returncode == 0:
            lines = r.stdout.splitlines()
            ident["version"] = lines[0].strip() if lines else ""
            ident["banner"] = lines[1].strip() if len(lines) > 1 else ""
    except Exception as e:                                  # noqa: BLE001
        ident["version"] = f"<unknown: {e}>"
    return ident


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", type=Path, action="append", default=[])
    ap.add_argument("--file", type=Path, action="append", default=[])
    ap.add_argument("--libtest", action="store_true")
    ap.add_argument("--libtest-limit", type=int, default=None)
    ap.add_argument("--stdlib", type=Path, default=None)
    ap.add_argument("--pyc", type=Path, default=None)
    ap.add_argument("--pyc-flag", action="append", default=[])
    ap.add_argument("--oracle", type=Path, default=None)
    ap.add_argument("--sysroot", type=Path, default=None)
    ap.add_argument("--jobs", "-j", type=int, default=4)
    ap.add_argument("--run-timeout", type=float, default=30.0)
    ap.add_argument("--compile-timeout", type=float, default=180.0)
    ap.add_argument("--json", type=Path, default=None)
    ap.add_argument("--show", type=int, default=10,
                    help="how many flagged cases to show evidence for")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--fail-on-silent-wrong", action="store_true",
                    help="exit 1 if any case exits 0 while its stdout differs "
                         "from CPython's (CHARTER I1). Needs no baseline: it is "
                         "a property of this run alone.")
    args = ap.parse_args()

    pyc = args.pyc or find_pyc()
    if pyc is None:
        print("error: no pyc binary (--pyc or $PYC_BINARY)", file=sys.stderr)
        return 2
    oracle = (args.oracle or corpus_mod.resolve_sysroot_oracle(args.sysroot)
              or Path(sys.executable))

    cases: list[Case] = []
    for d in args.corpus:
        cases += [Case(path=p) for p in sorted(d.glob("*.py"))
                  if p.is_file() and not p.name.startswith("_")]
    cases += [Case(path=f) for f in args.file]
    if args.libtest:
        stdlib = args.stdlib or Path(sysconfig.get_paths()["stdlib"])
        cases += list(corpus_mod.from_cpython_libtest(
            stdlib, oracle, limit=args.libtest_limit))
    if not cases:
        print("error: empty corpus", file=sys.stderr)
        return 2

    m = Measurer(oracle=oracle, compiler=Compiler(pyc, tuple(args.pyc_flag)),
                 run_timeout=args.run_timeout, compile_timeout=args.compile_timeout)

    if not args.quiet:
        print(f"{BOLD}pyc differential measurement{RST}")
        print(f"  subject {pyc}\n  oracle  {oracle}\n  cases   {len(cases)}\n")

    t0 = time.time()
    out: list[Measurement] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(m.measure, c): c for c in cases}
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            r = fut.result()
            out.append(r)
            done += 1
            if not args.quiet:
                tag = ",".join(r.flags) if r.flags else "ok"
                col = GRN if not r.flags else COLOUR.get(r.flags[0], DIM)
                print(f"  {done:>4}/{len(cases)}  {col}{tag:<38}{RST} {r.case.name}")
    elapsed = time.time() - t0
    out.sort(key=lambda r: (not r.impactful, r.case.name))
    return report(out, args, elapsed, pyc, oracle)


def report(out: list[Measurement], args, elapsed: float,
           pyc: Path, oracle: Path) -> int:
    total = len(out)
    counts = Counter(f for r in out for f in r.flags)
    passing = sum(1 for r in out if not r.impactful)
    clean = sum(1 for r in out if r.clean)
    rate = 100.0 * passing / total if total else 0.0

    print(f"\n{BOLD}Measured{RST}  ({elapsed:.1f}s, {total} cases)")
    for f in Flag.ALL:
        if counts[f]:
            mark = "impactful" if f in Flag.IMPACTFUL else "         "
            print(f"  {COLOUR.get(f, DIM)}{f:<20}{RST} {counts[f]:>5}   {DIM}{mark}{RST}")
    print(f"\n  {BOLD}pass rate  {rate:6.2f}%{RST}  ({passing}/{total} with no "
          f"impactful difference; {clean} byte-identical throughout)")

    norm = Counter(n for r in out for n in r.normalizers)
    if norm:
        varied = sum(1 for r in out if r.oracle_varied)
        print(f"\n  {DIM}volatile text collapsed before comparing "
              f"(both sides, identically):{RST}")
        for n, k in norm.most_common():
            print(f"    {DIM}{n:<16} {k:>5} case(s){RST}")
        print(f"    {DIM}{'':16} {varied:>5} case(s) had an oracle that varied, "
              f"so clock rules applied there{RST}")

    retried = [r for r in out
               for run in (r.oracle, r.oracle_again, r.subject)
               if run is not None and run.attempts > 1]
    if retried:
        print(f"\n  {DIM}{len(retried)} run(s) timed out once and were retried "
              f"at double the limit{RST}")

    if counts[Flag.ORACLE_UNSTABLE]:
        print(f"\n  {YEL}{counts[Flag.ORACLE_UNSTABLE]} case(s) where CPython "
              f"disagreed with CPython.{RST}")
        print(f"  {DIM}The program's own output changed between two runs of the "
              f"same interpreter.{RST}")

    if args.show:
        shown = [r for r in out if r.impactful][: args.show]
        if shown:
            print(f"\n{BOLD}Evidence{RST}")
            for r in shown:
                print(f"\n  {BOLD}{r.case.name}{RST}  [{', '.join(r.flags)}]")
                if r.diagnostic:
                    print(f"    {DIM}{r.diagnostic}{RST}")
                if Flag.STDOUT_DIFFERS in r.flags:
                    print(f"    {DIM}{r.first_stdout_diff()}{RST}")
                if Flag.EXIT_DIFFERS in r.flags and r.oracle and r.subject:
                    print(f"    {DIM}exit: cpython {r.oracle.exit} "
                          f"pyc {r.subject.exit}{RST}")

    if args.json:
        args.json.write_text(json.dumps({
            "schema": SCHEMA,
            "jobs": args.jobs,
            "oracle": oracle_identity(oracle),
            "subject": {"pyc": str(pyc), "pyc_flags": list(args.pyc_flag)},
            "total": total, "passing": passing, "clean": clean,
            "pass_rate": rate,
            "counts": {f: counts[f] for f in Flag.ALL if counts[f]},
            "results": [{
                "case": r.case.name,
                "path": str(r.case.path),
                "flags": r.flags,
                "diagnostic": r.diagnostic,
                "normalizers": r.normalizers,
                "oracle_varied": r.oracle_varied,
                "oracle_timed_out": bool(r.oracle and r.oracle.timed_out),
                "exit": ({"oracle": r.oracle.exit, "subject": r.subject.exit}
                         if r.oracle and r.subject else None),
            } for r in sorted(out, key=lambda x: x.case.name)],
        }, indent=2) + "\n")
        print(f"\n  wrote {args.json}")

    if args.fail_on_silent_wrong:
        # An unstable oracle disqualifies the case: with no reproducible ground
        # truth there is nothing for pyc to be wrong against.
        silent = [r for r in out
                  if Flag.STDOUT_DIFFERS in r.flags
                  and Flag.ORACLE_UNSTABLE not in r.flags
                  and r.subject is not None and r.subject.exit == 0]
        if silent:
            print(f"\n{RED}{BOLD}SILENT WRONG ANSWER (CHARTER I1){RST} — "
                  f"{len(silent)} case(s) exited 0 and printed something other "
                  f"than Python's answer:")
            for r in silent[:20]:
                print(f"  {r.case.name}  {DIM}{r.first_stdout_diff()}{RST}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
