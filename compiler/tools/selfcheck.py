"""Measurement scripts are assumed BROKEN until they demonstrate otherwise.

Five times in this rebuild a check reported success while checking nothing:

  * a sed probe whose pattern did not match, so it deleted nothing and
    "proved" exhaustiveness was unenforced when it is;
  * a well-formedness sweep over 400 files that lowered none of them and
    reported zero malformed blocks;
  * a curated steal list whose parameter names did not match the data, so
    three entries silently did not apply;
  * a coverage run that passed relative paths to a subprocess with a
    different cwd, so all 713 files "failed to parse";
  * a leak checker that counted `ret %N` as a leak, crying wolf instead.

The first four passed while measuring nothing; the fifth failed while
measuring wrongly. Both are the same defect: the result was believed without
evidence that the instrument works.

So: every measurement here proves it can produce BOTH answers before it is
allowed to report, and refuses outright on results too uniform to be real.
"""

from __future__ import annotations

import datetime
import os
import sys
from pathlib import Path
from typing import Callable, Iterable, Sequence

RED, GRN, YEL, DIM, BOLD, RST = (
    "\033[31m", "\033[32m", "\033[33m", "\033[90m", "\033[1m", "\033[0m")


# Refusals are DIAGNOSTICS, not metrics. A refused run says something true and
# useful -- "this instrument went blind" -- and that fact should be countable
# over time, not merely printed once and lost. It must never be folded into a
# metric, and absence of a number must never read as zero.
REFUSAL_LOG = Path(os.environ.get(
    "PYC_SELFCHECK_LOG",
    Path(__file__).resolve().parent.parent / "selfcheck-refusals.log"))


def record_refusal(label: str, reason: str) -> None:
    stamp = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    tool = Path(sys.argv[0]).name
    try:
        with open(REFUSAL_LOG, "a", encoding="utf-8") as f:
            f.write(f"{stamp}\t{tool}\t{label}\t{reason}\n")
    except OSError:
        pass          # never let bookkeeping break the refusal itself


class SelfCheckFailure(SystemExit):
    def __init__(self, msg: str, label: str = "self-check", reason: str = ""):
        record_refusal(label, reason or msg.strip().splitlines()[0].strip())
        super().__init__(f"{RED}{BOLD}self-check failed{RST}\n{msg}\n\n"
                         f"Refusing to report: an instrument that cannot be "
                         f"shown to work\nproduces numbers that mean nothing.\n"
                         f"Recorded to {REFUSAL_LOG.name} as a diagnostic.")


def require_detects(label: str, probe: Callable[[object], object],
                    bad: object, good: object, *, verbose: bool = True) -> None:
    """`probe` must find a problem in `bad` and none in `good`."""
    bad_result = probe(bad)
    good_result = probe(good)
    if not bad_result:
        raise SelfCheckFailure(
            f"  '{label}' found NO problem in an input known to be broken.\n"
            f"  It would therefore pass everything, including real defects.",
            label, "probe stale: known-bad input now passes")
    if good_result:
        raise SelfCheckFailure(
            f"  '{label}' reported a problem in an input known to be good:\n"
            f"    {good_result}\n"
            f"  A check that cries wolf gets ignored as surely as a silent one.",
            label, "false positive on a known-good input")
    if verbose:
        print(f"  {DIM}self-check: {label} detects a known defect and "
              f"passes a known-good input{RST}")


def reject_implausible_uniformity(outcomes: Sequence[str], *,
                                  what: str = "inputs",
                                  threshold: float = 0.98,
                                  min_n: int = 20) -> None:
    """Refuse a result where essentially everything failed the same way.

    A real subject under test fails in varied ways. One failure mode covering
    ~everything is far more likely to be the harness -- a wrong cwd, a missing
    binary, a bad path -- than a genuine property of the subject.
    """
    if len(outcomes) < min_n:
        return
    counts: dict[str, int] = {}
    for o in outcomes:
        counts[o] = counts.get(o, 0) + 1
    mode, n = max(counts.items(), key=lambda kv: kv[1])
    frac = n / len(outcomes)
    if frac >= threshold and mode != "ok":
        raise SelfCheckFailure(
            f"  {n}/{len(outcomes)} {what} ({frac:.0%}) produced the identical "
            f"outcome {mode!r}.\n"
            f"  That is far more likely to be a broken harness -- wrong cwd, "
            f"missing\n  binary, unresolved path -- than a property of the "
            f"subject under test.")


def require_nonempty(items: Iterable, what: str) -> list:
    out = list(items)
    if not out:
        raise SelfCheckFailure(f"  the {what} is empty, so nothing was measured.",
                               "empty-corpus", f"{what} was empty")
    return out
