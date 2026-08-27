#!/usr/bin/env python3
"""Compare two measurement records.

  ./verify/measure_compare.py --baseline base.json --current run.json
  ./verify/measure_compare.py --update --baseline base.json --current run.json

Two things fail the gate, and both are measured rather than judged:

  1. A case that PASSED now fails -- it had no impactful flag and now has one.
  2. A case that now exits 0 while its stdout differs from CPython's. That is
     a silent wrong answer (CHARTER I1), and it is a measurement, not a
     severity opinion: exit status and stdout are both recorded numbers.

A case that newly lost its GROUND TRUTH is reported but does not fail. Two
ways that happens, and pyc is not involved in either: the oracle disagreed with
itself across the two runs (ORACLE_UNSTABLE), or CPython did not finish even at
double the limit (TIMEOUT with the oracle dead). Both still count against the
pass rate -- the case genuinely cannot be scored -- but neither is a compiler
regression, and reporting one as such would be false.

Everything else is reported as CHANGED and fails nothing. A case that used to
be refused by the compiler and now runs and crashes has swapped one loud
failure for another; it did not pass before and does not pass now, and the
pass rate already says so.

That last part is the whole reason this file exists. The old gate ranked
verdicts by severity and compared ranks, so when a batch of compile errors got
FIXED and those files began running, the ranking said 48 regressions -- it
rated CRASH worse than COMPILE_ERROR. There is no ranking here to be wrong.

Exit codes:
  0  compared, nothing regressed
  1  compared, something regressed
  2  NOT compared -- the two runs are not comparable
  3  usage error
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

RED, GRN, YEL, DIM, BOLD, RST = (
    "\033[31m", "\033[32m", "\033[33m", "\033[90m", "\033[1m", "\033[0m")

STDOUT = "STDOUT_DIFFERS"
UNSTABLE = "ORACLE_UNSTABLE"
TIMEOUT = "TIMEOUT"
IMPACTFUL = frozenset({"DID_NOT_COMPILE", "ORACLE_UNSTABLE", "TIMEOUT",
                       STDOUT, "EXIT_DIFFERS"})


def gh(level: str, title: str, msg: str) -> None:
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::{level} title={title}::{msg.replace(chr(10), '%0A')}")


def summary(md: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(md.rstrip() + "\n\n")
    except OSError:
        pass


def not_comparable(reason: str, detail: str) -> int:
    print(f"\n{RED}{BOLD}{'=' * 60}{RST}")
    print(f"{RED}{BOLD}  NOT COMPARED — {reason}{RST}")
    print(f"{RED}{BOLD}{'=' * 60}{RST}")
    print(detail)
    print(f"\n{YEL}No comparison was made. This is not a regression report.{RST}")
    gh("error", f"NOT COMPARED — {reason}", detail)
    summary(f"## :no_entry: Not compared — {reason}\n\n```\n{detail}\n```")
    return 2


def flags_of(d: dict) -> dict[str, set[str]]:
    return {r["case"]: set(r["flags"]) for r in d["results"]}


def oracle_failed(d: dict) -> dict[str, bool]:
    """Cases where CPython itself produced no ground truth this run."""
    return {r["case"]: bool(r.get("oracle_timed_out")) for r in d["results"]}


def exits_of(d: dict) -> dict[str, int | None]:
    """The subject's exit status per case, for the I1 check."""
    return {r["case"]: (r["exit"] or {}).get("subject") for r in d["results"]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--current", type=Path)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--require-baseline", action="store_true")
    args = ap.parse_args()

    if args.update:
        if not args.current:
            print("error: --update needs --current", file=sys.stderr)
            return 3
        args.baseline.write_text(args.current.read_text())
        print(f"baseline updated from {args.current}")
        return 0
    if not args.current:
        print("error: --current required", file=sys.stderr)
        return 3
    if not args.baseline.exists():
        detail = f"no baseline at {args.baseline}"
        if args.require_baseline:
            return not_comparable("no baseline", detail)
        print(f"{YEL}no baseline — nothing verified{RST}")
        return 0

    base = json.loads(args.baseline.read_text())
    curr = json.loads(args.current.read_text())

    # Conditions that change what is measured. Each is a refusal, not a
    # verdict: comparing across them produces a number that means nothing.
    # A missing schema means a record from the OLD harness, whose fields this
    # comparator cannot read at all. Refuse rather than crash on the KeyError.
    bs, cs = base.get("schema"), curr.get("schema")
    if bs != cs:
        return not_comparable("measurement schema differs", (
            f"  baseline schema {bs if bs is not None else '<pre-schema record>'}\n"
            f"  current schema  {cs if cs is not None else '<pre-schema record>'}\n"
            "\n  Re-record the baseline with measure_run.py."))
    if base.get("jobs") != curr.get("jobs"):
        print(f"  {YEL}note{RST}: parallelism differs "
              f"({base.get('jobs')} vs {curr.get('jobs')})")

    # X.Y.Z only, never the -VV banner: CI builds its own sysroot, so the build
    # timestamp differs from a local one for the identical interpreter.
    bv = base.get("oracle", {}).get("version")
    cv = curr.get("oracle", {}).get("version")
    if bv and cv and bv != cv:
        return not_comparable("oracle differs",
                              f"  baseline CPython {bv}\n  current  CPython {cv}")
    bf = base.get("subject", {}).get("pyc_flags")
    cf = curr.get("subject", {}).get("pyc_flags")
    if bf is not None and cf is not None and bf != cf:
        return not_comparable("compiler flags differ",
                              f"  baseline {bf or '[]'}\n  current  {cf or '[]'}")

    bmap, cmap = flags_of(base), flags_of(curr)
    bexit, cexit = exits_of(base), exits_of(curr)
    cdead = oracle_failed(curr)
    shared = set(bmap) & set(cmap)

    regressed, silent, improved, changed, unstable = [], [], [], [], []
    for case in sorted(shared):
        was = bool(bmap[case] & IMPACTFUL)
        now = bool(cmap[case] & IMPACTFUL)
        if now and not was:
            gained = cmap[case] & IMPACTFUL
            # Same reasoning as UNSTABLE, one step further along: if CPython
            # itself did not finish, there is no ground truth this run and pyc
            # is not what failed. test_zipfile64 builds multi-gigabyte archives
            # and takes 107s under CPython here, against a 30s limit and a 60s
            # retry -- a statement about the limit, not the compiler.
            if gained == {UNSTABLE} or (gained == {TIMEOUT} and cdead.get(case)):
                # The compiler cannot cause this: ORACLE_UNSTABLE is computed
                # from two CPython runs with pyc nowhere in the picture. It
                # still counts against the pass rate -- the case has no ground
                # truth, and the rate should say so -- but failing the gate on
                # it would be reporting a property of the corpus as a compiler
                # regression. Measured on Lib/test, 9 of 14 lost matches in one
                # nightly were exactly this.
                unstable.append(case)
            else:
                regressed.append((case, sorted(gained)))
        elif was and not now:
            improved.append((case, sorted(bmap[case] & IMPACTFUL)))
        elif was and now and bmap[case] != cmap[case]:
            changed.append((case, sorted(bmap[case]), sorted(cmap[case])))

        # I1, measured: the binary claims success and prints the wrong thing.
        # New only -- one already in the baseline is a recorded defect, not a
        # regression, and the count can only go down.
        #
        # An unstable oracle disqualifies the case: if CPython does not agree
        # with CPython, there is no ground truth for pyc to be wrong AGAINST,
        # and "wrong answer" is not a claim the measurement supports. test_sort
        # seeds nothing and prints a different permutation every run; it was
        # reported here as a new I1 violation purely because its exit status
        # moved.
        if (STDOUT in cmap[case] and cexit.get(case) == 0
                and UNSTABLE not in cmap[case]
                and not (STDOUT in bmap[case] and bexit.get(case) == 0)):
            silent.append(case)

    removed = sorted(set(bmap) - set(cmap))
    added = sorted(set(cmap) - set(bmap))

    print(f"{BOLD}Comparison{RST}")
    print(f"  baseline  {base['pass_rate']:6.2f}%  ({base['passing']}/{base['total']})")
    print(f"  current   {curr['pass_rate']:6.2f}%  ({curr['passing']}/{curr['total']})")
    if added:
        print(f"  {GRN}+{len(added)} case(s) added{RST}")

    for case, fl in regressed[:20]:
        print(f"  {RED}now fails{RST}  {case}: {', '.join(fl)}")
    for case in silent[:20]:
        print(f"  {RED}silent wrong answer (I1){RST}  {case}: "
              f"exit 0, stdout differs")
    for case, fl in improved[:10]:
        print(f"  {GRN}now passes{RST} {case}: was {', '.join(fl)}")
    for case in unstable[:10]:
        why = ("CPython itself did not finish" if cdead.get(case)
               else "CPython disagreed with CPython")
        print(f"  {YEL}no ground truth{RST}  {case}: {why}"
              f"  {DIM}(not a compiler regression){RST}")
    for case, was, now in changed[:10]:
        print(f"  {DIM}changed{RST}   {case}: {','.join(was)} -> {','.join(now)}"
              f"  {DIM}(failed before and after; not a regression){RST}")

    failures = []
    if regressed:
        failures.append(f"{len(regressed)} case(s) that passed now fail")
    if silent:
        failures.append(f"{len(silent)} NEW silent wrong answer(s) (CHARTER I1): "
                        + ", ".join(silent[:5]))
    if removed:
        failures.append(f"{len(removed)} case(s) disappeared from the corpus: "
                        + ", ".join(removed[:5]))

    if failures:
        print(f"\n{RED}{BOLD}REGRESSION — compared, and something got worse{RST}")
        for f in failures:
            print(f"  - {f}")
        gh("error", "Regression", "; ".join(failures))
        summary("## :x: Regression\n\n" + "\n".join(f"- {f}" for f in failures))
        return 1

    if improved or added:
        bits = []
        if improved:
            bits.append(f"{len(improved)} case(s) lost an impactful flag")
        if added:
            bits.append(f"{len(added)} case(s) added")
        print(f"\n{GRN}{BOLD}BASELINE IS BEHIND{RST} — {'; '.join(bits)}. Refresh:")
        print(f"    ./verify/measure_compare.py --update "
              f"--baseline {args.baseline} --current {args.current}\n")
        gh("warning", "Baseline is behind", "; ".join(bits))

    print(f"{GRN}{BOLD}OK{RST} — compared, nothing regressed")
    summary(f"## :white_check_mark: Compared — no regression\n\n"
            f"`{base['pass_rate']:.2f}%` → `{curr['pass_rate']:.2f}%`")
    return 0


if __name__ == "__main__":
    sys.exit(main())
