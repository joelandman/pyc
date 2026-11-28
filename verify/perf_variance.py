#!/usr/bin/env python3
"""Characterise run-to-run perf variance so the timing gate can be a gate.

  ./verify/perf_variance.py --records /tmp/perf_var_*.json --json variance.json
  ./verify/perf_variance.py --set smoke --runs 8 --json verify/perf/variance.json

The nightly perf job (metric.yml) has always been warning-only: perf_compare's
±15% threshold is a single number, not a judgement, and the plan (next_steps
#3) said the timing gate arrives only once run-to-run variance is
characterised. This is that characterisation.

What it measures and why it is the right quantity. perf_compare already
compares the *speedup* (oracle/pyc median), which STATUS.md documents as the
machine-invariant quantity -- absolute seconds move ±49% between two runs on
the same box while the speedup moves ±5%. So the variance that gates the
speedup is the variance to characterise, and it transfers across host classes
(the gate compares speedups across hosts, never absolute time). What we pool
here is each workload's per-run speedup across N repeated runs of the smoke
set; the smoke set shares every workload with the nightly full set, so a
per-workload threshold characterised on smoke gates the full set too.

From N repeated runs, per workload: mean/std/robust-CV/min/max of the speedup, and a
per-workload gate threshold. From those, a per-class figure for a workload the
gate meets before it is characterised (a new real-app workload).

The threshold is a number derived from the data, not a guess:

    threshold = max(absolute_floor, spread_multiple * robust_CV)

`spread_multiple` is a sigma multiple (how many standard deviations the current
run may sit below the historical mean before it is a regression, not noise);
`absolute_floor` keeps a trivially stable workload (c-bound, ~1.00) from
gating at a sub-percent number that would red CI on a breath. Both are passed
in and recorded in the file so the number is auditable, per CHARTER I9.

The spread we gate on is the *robust* coefficient of variation (1.4826 * MAD /
median), not the plain std/CV: a single environmental blip -- a run
pre-empted by another process on a shared box -- must not set the gate. std is
that blip; MAD is the workload's genuine run-to-run spread. We keep std/CV in
the record too, so the number is auditable and the two are not hidden. Both
converge on the same threshold when the runs are clean (the common case on the
quiet CI runner, which re-characterises this file).

Exit codes: 0 the file was written (the gate is only as good as N); 2 not
enough runs to characterise anything (N < 4 leaves the CV untrustworthy, so we
refuse rather than emit a number that looks like a judgement).
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

BOLD, DIM, RED, YEL, GRN, RST = (
    "\033[1m", "\033[90m", "\033[31m", "\033[33m", "\033[32m", "\033[0m")


def _mean(xs: list[float]) -> float:
    return statistics.fmean(xs) if xs else 0.0


def _std(xs: list[float]) -> float:
    # sample std (n-1): the thing a gate is calibrated against is the spread
    # of future runs, not the descriptive spread of the ones we have.
    return statistics.stdev(xs) if len(xs) > 1 else 0.0


def _robust_scale(xs: list[float]) -> float:
    # Median absolute deviation, scaled to be consistent with std for
    # normal data. This is the gate's spread estimator: unlike std it is not
    # moved by a single environmental blip (a run pre-empted on a shared
    # box), which is exactly the failure mode we are guarding against.
    med = statistics.median(xs)
    devs = sorted(abs(x - med) for x in xs)
    n = len(devs)
    mad = devs[n // 2] if n % 2 else 0.5 * (devs[n // 2 - 1] + devs[n // 2])
    return 1.4826 * mad


def robust_cv(xs: list[float]) -> float:
    m = _mean(xs)
    return (_robust_scale(xs) / m) if m else 0.0


def _percentile(sorted_ts: list[float], q: float) -> float:
    if not sorted_ts:
        return 0.0
    return sorted_ts[int(q * (len(sorted_ts) - 1))]


def load_record(path: Path) -> dict:
    d = json.loads(path.read_text())
    if d.get("schema") != "pyc-perf/1":
        raise ValueError(f"{path}: schema {d.get('schema')!r} != pyc-perf/1")
    return d


def read_records(paths: list[Path]) -> list[dict]:
    recs = []
    for p in paths:
        try:
            recs.append(load_record(Path(p)))
        except (OSError, ValueError, json.JSONDecodeError) as e:
            print(f"  {YEL}skipping unreadable record{RST} {p}: {e}",
                  file=sys.stderr)
    return recs


def characterise(records: list[dict], *, set_name: str,
                 spread_multiple: float, absolute_floor: float) -> dict:
    # Pool each workload's speedup across the runs. A workload only counts if
    # it is OK (not DID_NOT_COMPILE / MISMATCH) with a real speedup in every
    # run we use it from -- a workload that flaked in one run is not evidence
    # of that run's variance, it is evidence of a broken run.
    by_name: dict[str, list[dict]] = {}
    cls_of: dict[str, str] = {}
    for r in records:
        for w in r.get("workloads", []):
            if w.get("status") != "OK" or not isinstance(w.get("speedup"), (int, float)):
                continue
            by_name.setdefault(w["name"], []).append(w["speedup"])
            cls_of[w["name"]] = w.get("cls", "unknown")

    if len(records) < 4:
        raise ValueError(
            f"only {len(records)} clean run(s); need >= 4 to characterise "
            f"variance (a CV from 3 samples is a guess, and I9 forbids gating "
            f"on a guess)")

    n_runs = len(records)
    by_workload: dict[str, dict] = {}
    cls_speedups: dict[str, list[float]] = {}
    cls_counts: dict[str, int] = {}
    for name, ups in by_name.items():
        m, s = _mean(ups), _std(ups)
        cv = (s / m) if m else 0.0
        rcv = robust_cv(ups)
        thr = max(absolute_floor, spread_multiple * rcv)
        by_workload[name] = {
            "class": cls_of[name],
            "runs": n_runs,
            "mean": round(m, 4),
            "std": round(s, 4),
            "cv": round(cv, 4),           # std-based, reported for audit
            "robust_cv": round(rcv, 4),  # used for the gate
            "min": round(min(ups), 4),
            "max": round(max(ups), 4),
            "threshold": round(thr, 4),
        }
        cls_speedups.setdefault(cls_of[name], []).extend(ups)
        cls_counts[cls_of[name]] = cls_counts.get(cls_of[name], 0) + 1

    by_class = {}
    for cls, ups in cls_speedups.items():
        m = _mean(ups)
        s = _std(ups)
        rcv = robust_cv(ups)
        thr = max(absolute_floor, spread_multiple * rcv)
        by_class[cls] = {
            "workloads": cls_counts[cls],
            "runs": n_runs,
            "mean": round(m, 4),
            "cv": round((s / m) if m else 0.0, 4),
            "robust_cv": round(rcv, 4),
            "threshold": round(thr, 4),
        }

    return {
        "schema": "pyc-perf-variance/1",
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "git": _git_sha(),
        "host": _host(),
        "set": set_name,
        "runs": n_runs,
        "threshold": {
            "spread_multiple": spread_multiple,
            "absolute_floor": absolute_floor,
        },
        "by_workload": by_workload,
        "by_class": by_class,
    }


def _git_sha() -> str:
    try:
        return subprocess.run(["git", "-C", Path(__file__).resolve().parent.parent,
                               "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def _host() -> dict:
    import platform
    import socket
    return {"name": socket.gethostname(), "machine": platform.machine(),
            "cpus": os.cpu_count()}
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--records", type=Path, action="append", default=[],
                    help="existing pyc-perf/1 records to pool (repeatable; "
                         "globs expand here). Reads stdin file names if omitted.")
    ap.add_argument("--set", choices=["smoke", "full"], default="smoke",
                    help="the set to characterise (default smoke). --runs "
                         "uses perf_run to produce them.")
    ap.add_argument("--runs", type=int, default=None,
                    help="run the smoke set this many times to characterise "
                         "(default: none -- read --records instead)")
    ap.add_argument("--json", type=Path, default=None,
                    help="where to write the variance record")
    ap.add_argument("--spread-multiple", type=float, default=3.0,
                    help="sigma multiple for the per-workload threshold "
                         "(default 3.0)")
    ap.add_argument("--absolute-floor", type=float, default=0.02,
                    help="minimum threshold, so a trivially stable workload "
                         "cannot gate at a sub-one-percent number "
                         "(default 0.02)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    records: list[dict]
    if args.runs:
        if args.runs < 4:
            print("error: --runs must be >= 4 to characterise variance",
                  file=sys.stderr)
            return 2
        records = _run_set(args.set, args.runs, args.quiet)
    else:
        paths: list[Path] = []
        for pat in (args.records or []):
            paths += sorted(glob.glob(str(pat)))
        if not paths:
            paths = [p for p in Path.cwd().glob("perf_var_*.json")]
        if not paths:
            print("error: no records to characterise (--records or --runs)",
                  file=sys.stderr)
            return 2
        records = read_records(paths)

    try:
        var = characterise(records, set_name=args.set,
                           spread_multiple=args.spread_multiple,
                           absolute_floor=args.absolute_floor)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(var, indent=2) + "\n")
        if not args.quiet:
            print(f"  wrote {args.json}")

    if not args.quiet:
        print(f"\n{BOLD}perf variance{RST}  {DIM}set {args.set} · "
              f"{var['runs']} runs · "
              f"threshold = max({args.absolute_floor:g}, "
              f"{args.spread_multiple:g}*CV){RST}")
        print(f"  {BOLD}{'workload':<15} {'mean':>8} {'cv':>7} "
              f"{'robust':>7} {'min':>8} {'max':>8} {'thr':>7}{RST}")
        for name, s in sorted(var["by_workload"].items()):
            col = ""
            print(f"  {name:<15} {s['mean']:>8.3f} "
                  f"{DIM}{s['cv']:>+7.1%}{RST} "
                  f"{s['robust_cv']:>+7.1%} {s['min']:>8.3f} "
                  f"{s['max']:>8.3f} {col}{s['threshold']:>7.1%}{RST}")
        print(f"\n  {BOLD}by class{RST}")
        for cls, s in sorted(var["by_class"].items()):
            print(f"    {cls:<12} mean {s['mean']:.3f} "
                  f"{DIM}cv {s['cv']:+.1%} · robust {s['robust_cv']:+.1%}{RST} "
                  f"thr {s['threshold']:.1%}")
        print(f"\n  {DIM}{len(var['by_workload'])} workloads characterised over "
              f"{var['runs']} runs on {var['host']['name']} "
              f"({var['host']['machine']} ×{var['host']['cpus']}) "
              f"({var['git'][:10] or '?'}). The gate (perf_compare --variance) "
              f"fails a workload when its current speedup is more than its "
              f"threshold below the baseline. Re-run on CI to characterise the "
              f"runner that gates; the speedup is the machine-invariant "
              f"quantity, so this transfers, but the number is what it was "
              f"measured on.{RST}")
    return 0


def _run_set(set_name: int, runs: int, quiet: bool) -> list[dict]:
    """Run the smoke/full set `runs` times, one record each, and read them
    back. Reuses perf_run (the authoritative runner) rather than duplicating
    its alternation/warmup logic. Each record is written to a temp file and
    pooled; the artifacts (--trace/--flame) are skipped -- this is variance,
    not the observation half of the perf plan."""
    import tempfile
    run_py = Path(__file__).resolve()
    tmp = tempfile.mkdtemp(prefix="pyc-perf-var-")
    records = []
    for i in range(runs):
        out = Path(tmp) / f"var_{i}.json"
        cmd = [sys.executable, str(run_py), "--set", set_name,
               "--no-artifacts", "--pyc-flag=-O2", "--json", str(out)]
        if quiet:
            cmd.append("--quiet")
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            print(f"  {RED}run {i} failed (exit {p.returncode}){RST}\n"
                  f"{p.stderr[-300:]}", file=sys.stderr)
        records.append(load_record(out))
    return records


if __name__ == "__main__":
    raise SystemExit(main())
