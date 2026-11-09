#!/usr/bin/env python3
"""Compare two perf records (schema pyc-perf/1) and report the drift.

  ./verify/perf_compare.py --current perf-smoke.json --baseline verify/perf/baseline.json

The comparability key is the one the plan fixed: workload pin hash, oracle
version, and pyc flags. A mismatch on any of them means the two records
measure DIFFERENT THINGS and no number in the table below it is a
comparison -- the comparator says so and exits 2. That is the harness
refusing to make a claim it cannot make (I9).

Hosts are deliberately NOT in the key: they are machine, not measurement
setup. When the hosts differ, absolute-time deltas are printed for the
record but only the SPEEDUP deltas alert, because a speedup measured
side-by-side on one machine is the machine-invariant quantity the table
publishes.

Alerts are warnings, not a gate: until run-to-run variance is
characterised (the plan's decision), a threshold crossing prints red and
exits 0. The threshold is a number, not a judgement; its provenance is in
STATUS.md with the first baseline.

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


def load(path: Path) -> dict:
    d = json.loads(path.read_text())
    if d.get("schema") != "pyc-perf/1":
        raise ValueError(f"{path}: schema {d.get('schema')!r} != pyc-perf/1")
    return d


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--current", type=Path, required=True)
    ap.add_argument("--baseline", type=Path, required=True)
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
        return {
            "pin_hash": rec.get("pin_hash"),
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

    by = {w["name"]: w for w in base["workloads"] if w.get("status") == "OK"}
    host_same = (cur.get("host", {}).get("class")
                 == base.get("host", {}).get("class"))

    print(f"{BOLD}perf drift{RST} {DIM}baseline {args.baseline} "
          f"-> current {args.current}{RST}")
    print(f"  {DIM}hosts: {base.get('host', {}).get('class')} vs "
          f"{cur.get('host', {}).get('class')}"
          f"{' (same class: absolute deltas alert)' if host_same else ' (different class: only speedups alert)'}{RST}")
    print(f"  {BOLD}{'workload':<15} {'base med':>10} {'now med':>10} "
          f"{'delta':>8} {'base sp':>8} {'now sp':>8} {'delta':>8}{RST}")

    alerts = 0
    for w in cur["workloads"]:
        b = by.get(w["name"])
        if w.get("status") != "OK":
            print(f"  {BOLD}{w['name']:<15}{RST} {RED}{w['status']}{RST}")
            continue
        if b is None:
            print(f"  {BOLD}{w['name']:<15}{RST} {DIM}new workload, no baseline{RST}")
            continue
        bm, nm = b["pyc"]["median"], w["pyc"]["median"]
        bs, ns = b["speedup"], w["speedup"]
        d_abs = (nm - bm) / bm if bm else 0.0
        d_sp = (ns - bs) / bs if bs else 0.0
        flag = ""
        if host_same and abs(d_abs) > args.threshold:
            flag = f" {RED}ALERT{RST}"
            alerts += 1
        elif not host_same and abs(d_sp) > args.threshold:
            flag = f" {RED}ALERT{RST}"
            alerts += 1
        col = RED if flag else (GRN if (host_same and d_abs < 0
                                        or not host_same and d_sp > 0) else "")
        print(f"  {BOLD}{w['name']:<15}{RST} {bm:>10.3f} {nm:>10.3f} "
              f"{col}{d_abs:>+7.1%}{RST} {bs:>8.2f} {ns:>8.2f} "
              f"{col}{d_sp:>+7.1%}{RST}{flag}")
    print()
    if alerts:
        print(f"  {RED}{alerts} workload(s) beyond +/-{args.threshold:.0%}{RST} "
              f"{DIM}(warning only: not a gate until variance is characterised){RST}")
    elif not args.quiet:
        print(f"  {DIM}no drift beyond +/-{args.threshold:.0%}{RST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
