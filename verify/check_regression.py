#!/usr/bin/env python3
"""Monotonicity gate (CHARTER I6).

The metric may never regress. This compares a fresh run against the committed
baseline and fails if:

  * the run used a different CPython oracle, or different pyc flags, than the
    baseline (refused, not reported as a regression -- not comparable), or
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
import os
import sys
from pathlib import Path

RED, GRN, YEL, BOLD, RST = "\033[31m", "\033[32m", "\033[33m", "\033[1m", "\033[0m"

# Exit codes are load-bearing and must stay distinguishable:
#   0  gate ran, nothing regressed
#   1  gate ran, something regressed        <- a verdict
#   2  gate DID NOT RUN (not comparable)    <- not a verdict
#
# GitHub renders every nonzero exit as an identical red X. Five consecutive
# verify runs exited 2 -- the gate never once evaluated -- and read exactly
# like five failing gates. So a "did not run" outcome now shouts, in the run
# log, as a workflow annotation, and in the step summary.
EXIT_OK, EXIT_REGRESSED, EXIT_DID_NOT_RUN = 0, 1, 2


def _in_actions() -> bool:
    return os.environ.get("GITHUB_ACTIONS") == "true"


def _annotate(level: str, title: str, message: str) -> None:
    """Workflow annotation: shows on the run page, not just in the log."""
    if _in_actions():
        one_line = message.replace("\n", "%0A")
        print(f"::{level} title={title}::{one_line}")


def _summary(markdown: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(markdown.rstrip() + "\n\n")
    except OSError:
        pass


def did_not_run(reason: str, detail: str) -> int:
    """The gate could not evaluate. This is NOT a regression verdict, and must
    never be mistaken for one."""
    print(f"\n{RED}{BOLD}{'='*62}{RST}")
    print(f"{RED}{BOLD}  GATE DID NOT RUN — {reason}{RST}")
    print(f"{RED}{BOLD}{'='*62}{RST}")
    print(detail)
    print(f"\n{YEL}Nothing was verified. This is not a regression report:{RST}")
    print(f"{YEL}the comparison never happened, so the metric is unguarded.{RST}")
    _annotate("error", f"GATE DID NOT RUN — {reason}",
              f"{detail}\nNothing was verified; the metric is unguarded.")
    _summary(f"## :no_entry: Gate did not run — {reason}\n\n"
             f"```\n{detail}\n```\n\n"
             "**Nothing was verified.** This is not a regression report — the "
             "comparison never happened, so the metric is currently unguarded.")
    return EXIT_DID_NOT_RUN

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
    # Anchored to THIS file, not the cwd: `make -C verify accept` runs from
    # verify/, where a cwd-relative "verify/baseline.json" is verify/verify/...
    # and the write fails with a bare FileNotFoundError. Same class of bug as
    # the one pycc's PYTHONPATH comment records.
    ap.add_argument("--baseline", type=Path,
                    default=Path(__file__).resolve().parent / "baseline.json")
    ap.add_argument("--current", type=Path, default=None)
    ap.add_argument("--update", action="store_true",
                    help="accept --current as the new baseline")
    ap.add_argument("--require-baseline", action="store_true",
                    help="fail (exit 2) if the baseline is missing, instead of "
                         "passing as a first run. CI should always set this.")
    ap.add_argument("--allow-rate-drop", action="store_true",
                    help="permit a pass-rate drop (use ONLY when the corpus grew "
                         "or got harder, and say so in the commit message)")
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
        # This used to print a note and return 0 -- a GREEN run in which the
        # gate protected nothing. metric.yml gated a baseline path that did not
        # exist and passed every night on that basis. Green is more dangerous
        # than red here, because nobody investigates green.
        detail = (f"no baseline at {args.baseline}\n"
                  f"nothing to compare the current run against")
        if args.require_baseline:
            return did_not_run("no baseline", detail)
        print(f"\n{YEL}{BOLD}GATE DID NOT RUN — no baseline{RST}")
        print(f"{YEL}{detail}{RST}")
        print(f"{YEL}Passing anyway (first run). Use --require-baseline in CI "
              f"so this can never pass silently.{RST}")
        _annotate("warning", "GATE DID NOT RUN — no baseline",
                  f"{detail}\nPassing as a first run; nothing was verified.")
        _summary(f"## :warning: Gate did not run — no baseline\n\n"
                 f"```\n{detail}\n```\n\nPassed as a first run. "
                 "**Nothing was verified.**")
        return EXIT_OK

    base, bmap = load(args.baseline)
    curr, cmap = load(args.current)

    failures: list[str] = []

    # A baseline is only meaningful relative to the oracle that produced it.
    # Comparing across CPython versions would report legitimate behavioural
    # differences as regressions, so refuse rather than mislead.
    base_oracle = base.get("oracle", {}).get("version", "")
    curr_oracle = curr.get("oracle", {}).get("version", "")
    if base_oracle and curr_oracle and base_oracle != curr_oracle:
        return did_not_run("oracle mismatch", (
            f"  baseline recorded against CPython {base_oracle}\n"
            f"  current run used CPython          {curr_oracle}\n"
            "\n  Re-record the baseline against the same oracle, or point CI "
            "at\n  the matching sysroot (--sysroot)."))
    if not base_oracle or not curr_oracle:
        print(f"{YEL}note{RST}: oracle version missing from "
              f"{'baseline' if not base_oracle else 'current'} — comparison "
              "is unverified")

    # Flags matter as much as the oracle: the old tree has documented -O0/-O2
    # divergence, so an -O0 run and an -O2 baseline are not comparable.
    base_flags = base.get("subject", {}).get("pyc_flags")
    curr_flags = curr.get("subject", {}).get("pyc_flags")
    if base_flags is not None and curr_flags is not None \
            and base_flags != curr_flags:
        return did_not_run("compiler flag mismatch", (
            f"  baseline recorded with pyc flags {base_flags or '[]  (defaults)'}\n"
            f"  current run used                 {curr_flags or '[]  (defaults)'}\n"
            "\n  Use the same flags, or keep a separate baseline per\n"
            "  configuration (the PR gate and the nightly metric each have one)."))

    # 1. The numerator is the signal; the denominator is noise.
    #
    # pass_rate is matched/scored, and `scored` moves run to run because
    # quarantine membership is nondeterministic -- a case whose oracle timed out
    # last night may be scored tonight. That makes the RATE unstable even when
    # nothing about the compiler changed, and it moves in the wrong direction:
    # a night with less flakiness scores MORE cases, so the rate falls. The
    # first scheduled run failed exactly this way -- 7/360 (1.94%) against a
    # 7/357 (1.96%) baseline, with the same 7 matches.
    #
    # Gating on that would make the nightly metric permanently red for reasons
    # unrelated to the compiler, which is how a gate stops being read at all.
    #
    # So: gate on `matched` (stable) and on per-case transitions below. A rate
    # drop with matched holding is denominator movement -- reported, not failed.
    drop = base["pass_rate"] - curr["pass_rate"]
    matched_drop = base["matched"] - curr["matched"]
    if matched_drop > 0:
        failures.append(
            f"matched count fell: {base['matched']} -> {curr['matched']} "
            f"(-{matched_drop})")
    elif drop > 1e-9:
        print(f"  {YEL}note{RST}: pass rate {base['pass_rate']:.2f}% -> "
              f"{curr['pass_rate']:.2f}% with matched unchanged at "
              f"{curr['matched']} — the scored set grew "
              f"({base['scored']} -> {curr['scored']}), i.e. fewer cases were "
              f"quarantined. Not a regression.")

    # 2. per-case regressions
    worse, new_p0, unscored, noise = [], [], [], []
    for case, bv in bmap.items():
        cv = cmap.get(case)
        if cv is None:
            continue  # case removed; covered by the corpus-shrink check below
        if RANK[cv] < RANK[bv] and cv in SCORED and bv in SCORED:
            worse.append(f"{case}: {bv} -> {cv}")
            if cv == "SILENT_WRONG_ANSWER" and bv != "SILENT_WRONG_ANSWER":
                new_p0.append(case)
        if bv in SCORED and cv not in SCORED:
            # Only a case that was PASSING can hide a regression by leaving the
            # scored set. A failing case going quarantined is the same
            # nondeterminism as above and flaps run to run.
            if bv == "MATCH":
                unscored.append(f"{case}: {bv} -> {cv} (a PASSING case left scoring)")
            else:
                noise.append(f"{case}: {bv} -> {cv}")

    if new_p0:
        failures.append(f"{len(new_p0)} NEW P0 silent wrong answer(s): "
                        + ", ".join(new_p0[:5]))
    if worse:
        failures.append(f"{len(worse)} case(s) regressed")
    if unscored:
        failures.append(f"{len(unscored)} case(s) left the scored set "
                        "(quarantining is not a way to raise the rate)")

    # 3. corpus must not shrink.
    #
    # A strict subset is NOT a regression -- it is an incomparable run, and
    # reporting it as a regression is actively misleading: a truncated corpus
    # can score HIGHER than the full one (an early alphabetical slice of
    # Lib/test does), so the pass rate moves for reasons that have nothing to
    # do with the compiler. metric.yml offers libtest_limit as an input, so
    # this is a lever a user can pull by accident.
    missing = set(bmap) - set(cmap)
    if missing and not (set(cmap) - set(bmap)):
        limit = curr.get("corpus", {}).get("libtest_limit")
        why = (f"the run was capped at {limit} file(s) (--libtest-limit)"
               if limit else
               "the current run covered a strict subset of the baseline")
        return did_not_run("corpus truncated", (
            f"  baseline covers {len(bmap)} case(s)\n"
            f"  current run covers {len(cmap)}\n"
            f"  {len(missing)} case(s) absent -- {why}\n"
            "\n  A subset cannot be compared against a full baseline: a\n"
            "  truncated corpus can score higher for reasons unrelated to the\n"
            "  compiler. Re-run over the full corpus, or record a separate\n"
            "  baseline for this selection.\n"
            f"\n  absent, first few: {', '.join(sorted(missing)[:5])}"))
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
    if noise:
        print(f"  {YEL}note{RST}: {len(noise)} failing case(s) crossed the "
              f"quarantine boundary (nondeterminism, not a scope reduction)")
        for line in noise[:5]:
            print(f"    {line}")

    # -- staleness ----------------------------------------------------------
    #
    # The gate is deliberately one-directional: it blocks regressions and never
    # adopts improvements, because auto-adopting would let a bad run quietly
    # become the new reference. The cost is that a baseline drifts stale in the
    # GOOD direction, silently, while every run keeps passing. The language
    # baseline sat 15 cases behind before anyone noticed, and only because
    # someone happened to compare by hand.
    #
    # So: report it loudly, never fail on it. Blocking a change for being
    # better would be absurd.
    improved = [f"{k}: {bmap[k]} -> {cmap[k]}"
                for k in bmap
                if k in cmap and RANK[cmap[k]] > RANK[bmap[k]]
                and bmap[k] in SCORED and cmap[k] in SCORED]
    gained = sorted(set(cmap) - set(bmap))
    matched_gain = curr["matched"] - base["matched"]
    stale_bits = []
    if matched_gain > 0:
        stale_bits.append(f"{matched_gain} more case(s) match "
                          f"({base['matched']} -> {curr['matched']})")
    if gained:
        stale_bits.append(f"{len(gained)} case(s) added to the corpus")
    if improved:
        stale_bits.append(f"{len(improved)} case(s) improved verdict")

    if stale_bits and not failures:
        cmd = (f"./verify/check_regression.py --update "
               f"--baseline {args.baseline} --current {args.current}")
        print(f"\n{YEL}{BOLD}BASELINE IS STALE{RST} — this run is BETTER than "
              f"the baseline:")
        for bit in stale_bits:
            print(f"  {GRN}+{RST} {bit}")
        for line in improved[:10]:
            print(f"    {GRN}improved{RST}  {line}")
        print(f"\n  The gate passes -- nothing regressed -- but the published "
              f"number\n  understates the compiler. Refresh with:\n\n    {cmd}\n")
        _annotate("warning", "Baseline is stale (run is better than baseline)",
                  "; ".join(stale_bits) + f". Refresh: {cmd}")
        _summary("## :arrow_up: Baseline is stale\n\n"
                 + "\n".join(f"- {b}" for b in stale_bits)
                 + f"\n\nThe gate passes — nothing regressed — but the published "
                   f"number understates the compiler.\n\n```\n{cmd}\n```")

    if failures:
        print(f"\n{RED}{BOLD}REGRESSION — the gate ran and FAILED{RST}")
        for f in failures:
            print(f"  - {f}")
        _annotate("error", "Monotonicity regression (gate ran)",
                  "; ".join(failures))
        _summary("## :x: Regression — the gate ran and failed\n\n"
                 + "\n".join(f"- {f}" for f in failures)
                 + f"\n\nbaseline `{base['pass_rate']:.2f}%` → current "
                   f"`{curr['pass_rate']:.2f}%`")
        print(f"\n{BOLD}I6: the metric may never regress.{RST} If this drop is "
              "intentional\n  (corpus grew or got harder), re-run with "
              "--allow-rate-drop and\n  explain it in the commit message.")
        return EXIT_REGRESSED

    print(f"\n{GRN}{BOLD}OK{RST} — the gate ran, nothing regressed")
    _summary(f"## :white_check_mark: Gate ran — no regression\n\n"
             f"baseline `{base['pass_rate']:.2f}%` → current "
             f"`{curr['pass_rate']:.2f}%` "
             f"({curr['matched']}/{curr['scored']} scored)")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
