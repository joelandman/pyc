#!/usr/bin/env python3
"""Monotonicity gate (CHARTER I6).

The metric may never regress. This compares a fresh run against the committed
baseline and fails if:

  * the run used a different CPython oracle than the baseline (refused, not
    reported as a regression -- they are simply not comparable), or
  * the pass rate dropped, or
  * any case got worse (MATCH -> anything, or a new P0), or
  * a case that used to be scored became quarantined (silent scope reduction).

The last one matters: quarantining a case removes it from the denominator, so
without this check the pass rate could be improved by making cases
nondeterministic rather than by making the compiler correct.

  ./verify/check_regression.py --baseline verify/baseline.json --current run.json
  ./verify/check_regression.py --update   # accept current as the new baseline
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

RED, GRN, YEL, BOLD, RST = "\033[31m", "\033[32m", "\033[33m", "\033[1m", "\033[0m"

# Lower is better; mirrors Verdict ordering in differential.py.
RANK = {
    "SILENT_WRONG_ANSWER": 0, "CRASH": 1, "HANG": 2, "COMPILE_ERROR": 3,
    "STDERR_DIFF": 4, "MATCH": 5,
    "QUARANTINE_NONDETERMINISTIC": 6, "QUARANTINE_ORACLE_FAILED": 7,
}
SCORED = {"SILENT_WRONG_ANSWER", "CRASH", "HANG", "COMPILE_ERROR",
          "STDERR_DIFF", "MATCH"}


def load(p: Path) -> tuple[dict, dict[str, str]]:
    d = json.loads(p.read_text())
    return d, {r["case"]: r["verdict"] for r in d["results"]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", type=Path, default=Path("verify/baseline.json"))
    ap.add_argument("--current", type=Path, default=None)
    ap.add_argument("--update", action="store_true",
                    help="accept --current as the new baseline")
    ap.add_argument("--allow-rate-drop", action="store_true",
                    help="permit a pass-rate drop (use ONLY when the corpus grew "
                         "or got harder, and say so in the commit message)")
    args = ap.parse_args()

    if args.update:
        if not args.current:
            print("error: --update needs --current", file=sys.stderr)
            return 2
        args.baseline.write_text(args.current.read_text())
        print(f"baseline updated from {args.current}")
        return 0

    if not args.current:
        print("error: --current required", file=sys.stderr)
        return 2
    if not args.baseline.exists():
        print(f"{YEL}no baseline at {args.baseline} — treating as first run{RST}")
        return 0

    base, bmap = load(args.baseline)
    curr, cmap = load(args.current)

    failures: list[str] = []

    # A baseline is only meaningful relative to the oracle that produced it.
    # Comparing across CPython versions would report legitimate behavioural
    # differences as regressions, so refuse rather than mislead.
    base_oracle = base.get("oracle", {}).get("version", "")
    curr_oracle = curr.get("oracle", {}).get("version", "")
    if base_oracle and curr_oracle and base_oracle != curr_oracle:
        print(f"{RED}{BOLD}ORACLE MISMATCH{RST}")
        print(f"  baseline recorded against CPython {base_oracle}")
        print(f"  current run used CPython          {curr_oracle}")
        print("\n  These are not comparable. Re-record the baseline against "
              "the same\n  oracle, or point CI at the matching sysroot "
              "(--sysroot).")
        return 2
    if not base_oracle or not curr_oracle:
        print(f"{YEL}note{RST}: oracle version missing from "
              f"{'baseline' if not base_oracle else 'current'} — comparison "
              "is unverified")

    # 1. pass rate
    drop = base["pass_rate"] - curr["pass_rate"]
    if drop > 1e-9 and not args.allow_rate_drop:
        failures.append(
            f"pass rate regressed: {base['pass_rate']:.2f}% -> "
            f"{curr['pass_rate']:.2f}% (-{drop:.2f})")

    # 2. per-case regressions
    worse, new_p0, unscored = [], [], []
    for case, bv in bmap.items():
        cv = cmap.get(case)
        if cv is None:
            continue  # case removed; covered by the corpus-shrink check below
        if RANK[cv] < RANK[bv] and cv in SCORED and bv in SCORED:
            worse.append(f"{case}: {bv} -> {cv}")
            if cv == "SILENT_WRONG_ANSWER" and bv != "SILENT_WRONG_ANSWER":
                new_p0.append(case)
        if bv in SCORED and cv not in SCORED:
            unscored.append(f"{case}: {bv} -> {cv} (dropped out of scoring)")

    if new_p0:
        failures.append(f"{len(new_p0)} NEW P0 silent wrong answer(s): "
                        + ", ".join(new_p0[:5]))
    if worse:
        failures.append(f"{len(worse)} case(s) regressed")
    if unscored:
        failures.append(f"{len(unscored)} case(s) left the scored set "
                        "(quarantining is not a way to raise the rate)")

    # 3. corpus must not shrink
    missing = set(bmap) - set(cmap)
    if missing:
        failures.append(f"{len(missing)} case(s) disappeared from the corpus: "
                        + ", ".join(sorted(missing)[:5]))

    print(f"{BOLD}Monotonicity check{RST}")
    print(f"  baseline  {base['pass_rate']:6.2f}%  ({base['matched']}/{base['scored']})")
    print(f"  current   {curr['pass_rate']:6.2f}%  ({curr['matched']}/{curr['scored']})")
    added = set(cmap) - set(bmap)
    if added:
        print(f"  {GRN}+{len(added)} new case(s) in corpus{RST}")

    for line in worse[:20]:
        print(f"  {RED}worse{RST}  {line}")
    for line in unscored[:20]:
        print(f"  {YEL}unscored{RST}  {line}")

    if failures:
        print(f"\n{RED}{BOLD}REGRESSION{RST}")
        for f in failures:
            print(f"  - {f}")
        print(f"\n{BOLD}I6: the metric may never regress.{RST} If this drop is "
              "intentional\n  (corpus grew or got harder), re-run with "
              "--allow-rate-drop and\n  explain it in the commit message.")
        return 1

    print(f"\n{GRN}{BOLD}OK{RST} — no regression")
    return 0


if __name__ == "__main__":
    sys.exit(main())
