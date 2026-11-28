#!/usr/bin/env python3
"""Compare two perf records (schema pyc-perf/1) and report the drift.

  ./verify/perf_compare.py --current perf-smoke.json --baseline verify/perf/baseline.json

The comparability key has two levels:
  * record level: oracle version and pyc flags. Both records must agree,
    whatever set was run;
  * workload level: for every workload name present in BOTH records, the
    pin (path, argv, workload-file content hash) must be identical. A
    changed constant in a workload file is a different workload.

The two levels meet, not the sets: a set can be compared against a
baseline recorded for a DIFFERENT set (the nightly full set against the
smoke baseline) as long as the intersection is pin-identical. New
workloads (no baseline entry) are listed, not compared. A mismatch at
either level means the two records measure DIFFERENT THINGS and no number
in the table is a comparison -- the comparator says so and exits 2. That
is the harness refusing to make a claim it cannot make (I9).

Hosts are deliberately NOT in the key: they are machine, not measurement
setup. When the hosts differ, absolute-time deltas are printed for the
record but only the SPEEDUP deltas alert, because a speedup measured
side-by-side on one machine is the machine-invariant quantity the table
publishes.

Alerts are warnings, not a gate, UNLESS a variance model is supplied
(--variance): with the model, a workload regressing beyond its own
characterised spread fails the run (exit 1); without it, a threshold
crossing prints red and exits 0. The threshold is a number, not a
judgement; the variance model's provenance (how many runs it was measured
over, on which host) is in the file itself and in next_steps.md.

Exit codes: 0 comparable (alerts may be present); 2 not comparable or
unreadable records.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

BOLD, DIM, RED, YEL, GRN, RST = (
    "\033[1m", "\033[90m", "\033[31m", "\033[33m", "\033[32m", "\033[0m")


def read_json(path: Path) -> dict:
    return json.loads(path.read_text())


def load(path: Path) -> dict:
    d = read_json(path)
    if d.get("schema") != "pyc-perf/1":
        raise ValueError(f"{path}: schema {d.get('schema')!r} != pyc-perf/1")
    return d


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--current", type=Path, required=True)
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--variance", type=Path, default=None,
                    help="per-workload variance model (pyc-perf-variance/1) "
                         "that turns the timing drift into a real gate: a "
                         "workload regressing beyond its characterised spread "
                         "fails the run. Without it, alerts stay warnings.")
    ap.add_argument("--threshold", type=float, default=0.15,
                    help="relative drift that raises an alert (default 0.15; "
                         "provisional until variance is characterised)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    try:
        cur = load(args.current)
        base = load(args.baseline)
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    def key(rec: dict) -> dict:
        # Record-level key: what BOTH records must agree on, whatever set
        # was run. (The set's pin_hash identifies the set, it does not gate
        # a comparison -- see the per-workload pin pass below.)
        return {
            "oracle_version": rec.get("oracle", {}).get("version"),
            "pyc_flags": sorted(rec.get("pyc", {}).get("flags", [])),
        }

    kc, kb = key(cur), key(base)
    drift = [f"{k}: baseline {kb[k]!r} -> current {kc[k]!r}"
             for k in kc if kb.get(k) != kc[k]]
    if drift:
        print(f"{RED}not comparable{RST} {DIM}({args.baseline} vs {args.current}){RST}")
        for line in drift:
            print(f"  {line}")
        print("  a baseline is re-recorded only with a measured reason in the "
              "commit; a drifted key means the comparison would measure two "
              "different things.")
        return 2

    # Per-workload pin: every name in the intersection must be the SAME
    # workload (path, argv, file content). A baseline record without
    # content_sha is a pre-content-pin record and cannot be matched.
    base_by_name = {w["name"]: w for w in base["workloads"]}
    pin_drift = []
    for w in cur["workloads"]:
        b = base_by_name.get(w["name"])
        if b is None:
            continue  # new workload: nothing to pin against
        for field in ("path", "argv", "content_sha"):
            if b.get(field) != w.get(field):
                pin_drift.append(
                    f"  {w['name']}: {field} drifted: baseline "
                    f"{b.get(field)!r} -> current {w.get(field)!r}")
                break
    if pin_drift:
        print(f"{RED}not comparable{RST} {DIM}({args.baseline} vs {args.current}){RST}")
        for line in pin_drift:
            print(line)
        print("  a workload whose pinned input changed (or a baseline "
              "recorded before file contents were pinned) is a DIFFERENT "
              "workload: comparing it would measure two different things. "
              "Re-record the baseline with a measured reason in the commit.")
        return 2

    by = {w["name"]: w for w in base["workloads"] if w.get("status") == "OK"}
    host_same = (cur.get("host", {}).get("class")
                 == base.get("host", {}).get("class"))

    # A per-workload variance model turns the timing gate from trend-only into
    # a real one: a workload may regress by up to its own measured spread
    # (std of its speedup across repeated runs) without tripping the gate, and
    # a new workload falls back to its class. Without the model the fixed
    # +/-threshold stays a warning (the number, not the judgement).
    var_workload: dict[str, dict] = {}
    var_class: dict[str, dict] = {}
    var_thresholds: dict[str, float] = {}
    if args.variance:
        try:
            v = read_json(args.variance)
        except (OSError, ValueError, json.JSONDecodeError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        if v.get("schema") != "pyc-perf-variance/1":
            print(f"error: {args.variance}: schema {v.get('schema')!r} != "
                  "pyc-perf-variance/1", file=sys.stderr)
            return 2
        var_workload = v["by_workload"]
        var_class = v["by_class"]
        for w in cur["workloads"]:
            if w.get("status") != "OK":
                continue
            thr = var_workload.get(w["name"], {}).get("threshold")
            if thr is None:  # not yet characterised: use the class figure
                thr = var_class.get(w.get("cls", ""), {}).get("threshold")
            if thr is not None:
                var_thresholds[w["name"]] = thr
    variance_active = bool(var_thresholds)
    variance_runs = v.get("runs") if variance_active else None

    print(f"{BOLD}perf drift{RST} {DIM}baseline {args.baseline} "
          f"-> current {args.current}{RST}")
    print(f"  {DIM}pin: baseline {base.get('pin_hash')} vs current "
          f"{cur.get('pin_hash')} (set identity; the per-workload pins "
          f"matched){RST}")
    print(f"  {DIM}hosts: {base.get('host', {}).get('class')} vs "
          f"{cur.get('host', {}).get('class')}"
          f"{' (same class: absolute deltas alert)' if host_same else ' (different class: only speedups alert)'}{RST}")
    if variance_active:
        print(f"  {BOLD}variance gate{RST} {DIM}per-workload, from "
              f"{variance_runs} runs on {v['host'].get('name', 'this host')}: "
              f"threshold = max({v['threshold']['absolute_floor']:g}, "
              f"{v['threshold']['spread_multiple']:g}*CV); "
              f"a workload regressing beyond its threshold fails the gate{RST}")
    print(f"  {BOLD}{'workload':<15} {'class':<12} {'base sp':>8} "
          f"{'now sp':>8} {'d_sp':>9}{RST}")

    alerts = 0
    failures = 0
    for w in cur["workloads"]:
        b = by.get(w["name"])
        if w.get("status") != "OK":
            print(f"  {BOLD}{w['name']:<15}{w.get('cls',''):<12}{RST} "
                  f"{RED}{w['status']}{RST}")
            continue
        if b is None:
            print(f"  {BOLD}{w['name']:<15}{w.get('cls',''):<12}{RST} "
                  f"{DIM}new workload, no baseline{RST}")
            continue
        bm, nm = b["pyc"]["median"], w["pyc"]["median"]
        bs, ns = b["speedup"], w["speedup"]
        d_abs = (nm - bm) / bm if bm else 0.0
        d_sp = (ns - bs) / bs if bs else 0.0
        flag = ""
        col = ""
        if variance_active and w["name"] in var_thresholds:
            thr = var_thresholds[w["name"]]
            if d_sp < -thr:
                flag = f" {RED}REGRESSION{RST}"
                failures += 1
                col = RED
            elif host_same and abs(d_abs) > args.threshold:
                flag = f" {RED}ALERT{RST}"
                alerts += 1
                col = RED
            else:
                col = GRN if d_sp > 0 else ""
        elif host_same and abs(d_abs) > args.threshold:
            flag = f" {RED}ALERT{RST}"
            alerts += 1
            col = RED
        elif not host_same and abs(d_sp) > args.threshold:
            flag = f" {RED}ALERT{RST}"
            alerts += 1
            col = RED
        else:
            col = GRN if (host_same and d_abs < 0 or not host_same and d_sp > 0) else ""
        thr_txt = f"  thr {var_thresholds.get(w['name'], 0):.0%}" if variance_active else ""
        print(f"  {BOLD}{w['name']:<15}{RST} {bm:>10.3f} {nm:>10.3f} "
              f"{col}{d_sp:>+8.1%}{RST} {bs:>8.2f} {ns:>8.2f} "
              f"{col}{d_sp:>+8.1%}{RST}{thr_txt}{flag}")
    print()
    if failures:
        print(f"  {RED}{failures} workload(s) regressed beyond their "
              f"characterised variance -- the perf gate has FAILED{RST}")
        return 1
    if alerts:
        what = ("absolute-time drift without a speedup regression "
                if variance_active else "drift beyond +/-")
        print(f"  {RED}{alerts} workload(s) {what}"
              f"{'' if variance_active else f'{args.threshold:.0%}'}{RST} "
              f"{DIM}(warning: same-machine load moved, pyc kept pace with the "
              f"oracle -- not a gate failure){RST}")
    elif not args.quiet:
        print(f"  {DIM}no drift beyond +/-{args.threshold:.0%}{RST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
