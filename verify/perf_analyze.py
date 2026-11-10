#!/usr/bin/env python3
"""Analyse the perf artifacts: WHERE do the cycles live?

  ./verify/perf_analyze.py --artifacts-dir perf-artifacts

This is the observation half of the perf plan (question A). For each
workload's --trace and --flamegraph artifacts it reports:

  * flamegraph frames: self/total sample share per function (the SVG
    writer's own accounting);
  * trace accounting: per-function self time, the share of time in C
    slices (cat pyc.ccall, time that leaves generated code), and the
    share outside any window (module level + startup);
  * the top C functions by time -- this is the direct evidence for
    codegen targets (PyObject_GetAttr in a hot path is an inlineable
    target; time in C work is the floor).

The two artifacts are independent measurements of the same run (sampling
vs event tracing) and are printed side by side precisely so a
disagreement is visible: 98% self in one and 60% in the other means one
of them is being fooled, not that the program changed.

Trace window kinds (both properly nested in time; self = window minus
windows strictly inside it):
  * B/E pairs for ordinary function frames;
  * complete X events, two cats: pyc.ccall (a call into C) and pyc.func
    (a GENERATOR segment: a generator's step is a suspend/resume and two
    B's before an E is invalid nesting, so the writer emits the segment
    as a complete X -- TRACING.md).

Coverage note, measured 2026-11-09: in arithmetic-dense workloads the
hot loop is native and makes almost no monitoring events (nbody: 24
events for a 500k-iteration run), so there the flamegraph is the
observation; in generator-dense workloads every generator step is an X
event and the trace is infeasible (matmul: 3.6 GB for a 1 s run) -- the
flamegraph is the observation there too. The trace carries the per-
function and C: attribution for the call-dense class, where it stays
small (ast_parse: 6 MB, xml_build: 51 MB).

Nothing here judges correctness or speedup: it only answers "where".
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path

TITLE_RE = re.compile(
    r"<title>([^:]*): (\d+) samples \(([\d.]+)% self, ([\d.]+)% total\)</title>")
# Frame names can carry XML entities (<module> is &lt;module&gt;).
UNESC = {"&lt;": "<", "&gt;": ">", "&amp;": "&", "&quot;": '"', "&apos;": "'"}


def unesc(s: str) -> str:
    for k, v in UNESC.items():
        s = s.replace(k, v)
    return s


def flame(path: Path) -> list[tuple[str, float, float, int]]:
    """(name, self_pct, total_pct, samples) per frame, most samples first."""
    frames = []
    for m in TITLE_RE.finditer(path.read_text()):
        frames.append((unesc(m.group(1)), float(m.group(3)),
                       float(m.group(4)), int(m.group(2))))
    frames.sort(key=lambda f: f[3], reverse=True)
    return frames


def trace(path: Path, max_bytes: int = 256 * 1024 * 1024) -> dict | None:
    """Self-time accounting over the event windows of one trace (ms).

    Returns None when the trace exceeds max_bytes: generator-dense
    workloads make event traces infeasible (see module docstring); the
    flamegraph is the observation for those.
    """
    if path.stat().st_size > max_bytes:
        return None

    open_at: list[tuple[str, float]] = []   # stack: recursive calls nest
    windows: list[tuple[float, float, str, bool]] = []  # start, end, name, is_c
    t0 = t1 = None
    with open(path) as fh:
        for line in fh:
            e = json.loads(line)
            ph = e.get("ph")
            ts = e.get("ts")
            if ph == "B":
                open_at.append((e["name"], ts))
            elif ph == "E":
                if not open_at or open_at[-1][0] != e["name"]:
                    raise AssertionError(f"unbalanced E: {e['name']} "
                                         f"(top: {open_at[-1][0] if open_at else None})")
                name, start = open_at.pop()
                windows.append((start, ts, name, False))
            elif ph == "X":
                end = ts + e.get("dur", 0.0)
                is_c = e.get("cat") == "pyc.ccall"
                name = e["name"][2:] if is_c else e["name"]
                windows.append((ts, end, name, is_c))
            else:
                continue
            if t0 is None or ts < t0:
                t0 = ts
            if t1 is None or (ph == "X" and end > t1) or (ph in ("B", "E") and ts > t1):
                t1 = ts if ph != "X" else end
    if t0 is None:
        return None

    # Stack pass over the time-ordered windows: a window is a child of the
    # innermost still-open window. Ties on start: the longer window first,
    # it is the parent. Durations are microseconds; a popped window's self
    # is its duration minus the child sums accumulated while it was open.
    windows.sort(key=lambda w: (w[0], -(w[1] - w[0])))
    self_ms: Counter = Counter()         # func windows: self time
    c_ms: Counter = Counter()            # C windows: total time
    c_self_ms: Counter = Counter()       # C windows: self time (no nesting)
    stack: list[list] = []               # [end, start, child_us, name, is_c]
    for start, end, name, is_c in windows:
        dur = end - start
        while stack and stack[-1][0] < start:
            pe, pstart, pchild, pname, pcat = stack.pop()
            if pcat:
                c_self_ms[pname] += (pe - pstart - pchild) / 1000.0
            else:
                self_ms[pname] += (pe - pstart - pchild) / 1000.0
        if stack:
            stack[-1][2] += dur
        if is_c:
            c_ms[name] += dur / 1000.0
        stack.append([end, start, 0.0, name, is_c])
    while stack:
        pe, pstart, pchild, pname, pcat = stack.pop()
        if pcat:
            c_self_ms[pname] += (pe - pstart - pchild) / 1000.0
        else:
            self_ms[pname] += (pe - pstart - pchild) / 1000.0

    total_ms = (t1 - t0) / 1000.0
    inside_ms = sum(self_ms.values()) + sum(c_self_ms.values())
    module_ms = max(0.0, total_ms - inside_ms)
    return {
        "total_ms": total_ms,
        "self_ms": self_ms,
        "c_ms": c_ms,
        "c_self_ms": c_self_ms,
        "module_ms": module_ms,
        "inside_ms": inside_ms,
    }


def pct(x: float, total: float) -> str:
    return f"{100.0 * x / total:5.1f}%" if total else "  -  "


def report(wl: str, adir: Path) -> None:
    tp = adir / wl / f"{wl}.trace.ndjson"
    fp = adir / wl / f"{wl}.flame.svg"
    print(f"\n{wl}")
    if not tp.exists() and not fp.exists():
        print("  (no artifacts)")
        return
    if fp.exists():
        print("  flamegraph (99 Hz sampling):")
        for name, sp, tp_, n in flame(fp)[:10]:
            if name == "all":
                continue
            print(f"    {name:<42} {sp:5.1f}% self  {tp_:5.1f}% total  {n:4d} samples")
    if tp.exists():
        t = trace(tp)
        if t is None:
            print(f"  trace: {tp.stat().st_size / 1e6:.0f} MB -- too large to "
                  f"parse (generator-dense; see module docstring). Flamegraph "
                  f"carries the observation.")
            return
        tot = t["total_ms"]
        c_self_tot = sum(t["c_self_ms"].values())
        print(f"  trace (events): total {tot:.0f} ms | inside windows "
              f"{pct(t['inside_ms'], tot)} | C self {pct(c_self_tot, tot)} | "
              f"module+startup {pct(t['module_ms'], tot)}")
        print("    top functions by self time:")
        for name, ms in t["self_ms"].most_common(8):
            print(f"    {name:<42} {ms:8.1f} ms  {pct(ms, tot)}")
        print("    top C: functions by time (total; self):")
        for name, ms in t["c_ms"].most_common(8):
            print(f"    {name:<42} {ms:8.1f} ms  {pct(ms, tot)}"
                  f"  self {t['c_self_ms'].get(name, 0.0):8.1f} ms")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--artifacts-dir", type=Path, required=True)
    ap.add_argument("--workload", action="append", default=None,
                    help="restrict to these workloads (repeatable)")
    args = ap.parse_args()

    dirs = sorted(p for p in args.artifacts_dir.iterdir() if p.is_dir())
    if args.workload:
        dirs = [p for p in dirs if p.name in args.workload]
    for d in dirs:
        report(d.name, args.artifacts_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
