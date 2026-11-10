#!/usr/bin/env python3
"""Run real-world performance workloads: pyc-compiled vs sysroot CPython.

  ./verify/perf_run.py --set smoke --json perf-smoke.json
  ./verify/perf_run.py --set full --artifacts-dir /tmp/pa --json perf-full.json

CHARTER I5 carries over in full: the oracle is the sysroot CPython (the
--oracle / --sysroot discipline of measure_run), the environment both sides
run under is the pinned child_env, and nothing the programs produce is
rewritten. This harness measures time and exit status; it does not judge
correctness against an expected string. It DOES cross-check the two sides
against each other (same exit, same stdout): a workload whose pyc and
oracle disagree is flagged MISMATCH, because a timing comparison over a
wrong program is a coin-flip (I9).

Per workload:
  * compile pyc once, with the pinned flags (default -O2);
  * 1 warmup run per side (discarded: page cache, allocator warm state);
  * 7 timed runs per side, alternated, wall clock around the whole process
    (startup included, symmetric on both sides);
  * median + p25/p75 (nearest rank over the sorted samples);
  * one extra build with --trace --flamegraph, run once under PYC_TRACE
    and once under PYC_FLAMEGRAPH, so every workload ships a Perfetto trace
    and an SVG flamegraph (TRACING.md). Artifacts are measurements, not
    correctness evidence: the traced/flame runs are not in the timed set.

Exit codes: 0 all workloads clean; 1 JSON written but at least one workload
flagged (DID_NOT_COMPILE / RUN_FAILED / MISMATCH / ALL_TIMED_OUT); 2
configuration error, no JSON.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from verify import perf as perf_mod           # noqa: E402
from verify.measure import child_env          # noqa: E402
from verify.measure_run import oracle_identity  # noqa: E402

BOLD, DIM, RED, YEL, GRN, RST = (
    "\033[1m", "\033[90m", "\033[31m", "\033[33m", "\033[32m", "\033[0m")

SCHEMA = "pyc-perf/1"


def find_pyc() -> Path | None:
    """Same resolution order as measure_run.find_pyc: $PYC_BINARY, then the
    built driver, from any cwd."""
    if (env := os.environ.get("PYC_BINARY")) and Path(env).exists():
        return Path(env)
    repo = Path(__file__).resolve().parent.parent
    for c in ("compiler/tools/pyc", repo / "compiler" / "tools" / "pyc"):
        if Path(c).exists():
            return Path(c).resolve()
    return None


def host_identity() -> dict:
    """Where this ran. The CI runner and a laptop are different machines;
    a trend is only a trend within one machine class, so the record says
    which one it is. perf_compare uses this to decide whether absolute-time
    deltas are meaningful at all."""
    return {
        "name": socket.gethostname(),
        "machine": platform.machine(),
        "cpus": os.cpu_count(),
        "ci": "GITHUB_ACTIONS" in os.environ,
        "class": ("gha-" + os.environ.get("RUNNER_IMAGE_NAME", "runner")
                  if "GITHUB_ACTIONS" in os.environ else "local"),
    }


def git_identity() -> dict:
    repo = Path(__file__).resolve().parent.parent
    try:
        sha = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=10).stdout.strip()
        return {"sha": sha}
    except (OSError, subprocess.SubprocessError):
        return {"sha": ""}


def _percentile(sorted_ts: list[float], q: float) -> float | None:
    """Nearest-rank over sorted samples: p25 of 7 = index 1, p75 = index 5.
    Deterministic, no interpolation to invent a number the machine never
    produced."""
    if not sorted_ts:
        return None
    return sorted_ts[int(q * (len(sorted_ts) - 1))]


def _timed_run(cmd: list[str], *, cwd: Path, env: dict[str, str],
               timeout: float) -> dict:
    """One wall-clocked process run. The timer wraps the WHOLE process
    (spawn + startup + work + exit), symmetric on both sides. A timeout
    discards the sample (timed_out) but never the run: 6 good samples are
    still 6 good samples."""
    t0 = time.perf_counter()
    try:
        p = subprocess.run(cmd, cwd=cwd, input="", capture_output=True,
                           text=True, timeout=timeout, env=env,
                           errors="replace")
    except subprocess.TimeoutExpired:
        return {"timed_out": True, "exit": None, "stdout": "", "stderr": "",
                "elapsed": None}
    except OSError as e:
        return {"timed_out": False, "exit": 127, "stdout": "",
                "stderr": f"exec failed: {e}", "elapsed": None}
    el = time.perf_counter() - t0
    return {"timed_out": False, "exit": p.returncode, "stdout": p.stdout,
            "stderr": p.stderr, "elapsed": el}


class Runner:
    def __init__(self, *, pyc: Path, oracle: Path,
                 pyc_flags: tuple[str, ...], runs: int, warmup: int,
                 artifacts_dir: Path | None, compile_timeout: float,
                 quiet: bool):
        self.pyc = pyc
        self.oracle = oracle
        self.pyc_flags = pyc_flags
        self.runs = runs
        self.warmup = warmup
        self.artifacts_dir = artifacts_dir
        self.compile_timeout = compile_timeout
        self.quiet = quiet
        self.build = Path(tempfile.mkdtemp(prefix="pyc-perf-build-"))

    def _log(self, *a) -> None:
        if not self.quiet:
            print(*a)

    def compile(self, src: Path, out: Path, extra: list[str]) -> dict:
        env = child_env()
        t0 = time.perf_counter()
        p = subprocess.run([str(self.pyc), str(src), "-o", str(out),
                            *self.pyc_flags, *extra],
                           capture_output=True, text=True,
                           timeout=self.compile_timeout, env=env)
        el = time.perf_counter() - t0
        return {"ok": p.returncode == 0, "elapsed": el,
                "stderr": p.stderr[-400:] if p.returncode else ""}

    def workload(self, w: perf_mod.Workload) -> dict:
        cwd = Path(tempfile.mkdtemp(prefix=f"pyc-perf-{w.name}-"))
        env = child_env()
        env["TMPDIR"] = env["TEMP"] = env["TMP"] = str(cwd)
        record = {
            "name": w.name, "cls": w.cls, "path": w.relpath,
            "argv": list(w.argv), "timeout": w.timeout,
            "content_sha": w.content_sha,
            "flags": list(self.pyc_flags),
        }
        plain = self.build / f"{w.name}"
        c = self.compile(w.path, plain, [])
        record["compile_s"] = c["elapsed"]
        if not c["ok"]:
            record.update(status="DID_NOT_COMPILE", compile_err=c["stderr"])
            self._log(f"  {BOLD}{w.name}{RST} {RED}DID_NOT_COMPILE{RST}")
            return record

        sides = {"pyc": [str(plain), *w.argv],
                 "oracle": [str(self.oracle), str(w.path), *w.argv]}
        for side in ("pyc", "oracle"):
            r = _timed_run(sides[side], cwd=cwd, env=env,
                           timeout=w.timeout * 2)  # warmup may hit a cold cache
            if r["timed_out"] or r["exit"] != 0:
                record.update(status="RUN_FAILED",
                              fail=f"{side} warmup exit={r['exit']} "
                                   f"timeout={r['timed_out']}")
                return record

        samples: dict[str, list[float]] = {"pyc": [], "oracle": []}
        timeouts: dict[str, int] = {"pyc": 0, "oracle": 0}
        last: dict[str, dict] = {}
        for _ in range(self.runs):
            for side in ("pyc", "oracle"):
                r = _timed_run(sides[side], cwd=cwd, env=env,
                               timeout=w.timeout)
                last[side] = r
                if r["timed_out"]:
                    timeouts[side] += 1
                elif r["elapsed"] is not None:
                    samples[side].append(r["elapsed"])

        def stat(side: str) -> dict:
            ts = sorted(samples[side])
            return {
                "n": len(ts),
                "times": [round(t, 6) for t in ts],
                "median": round(statistics.median(ts), 6) if ts else None,
                "p25": _percentile(ts, 0.25),
                "p75": _percentile(ts, 0.75),
                "timeouts": timeouts[side],
                "exit": last[side]["exit"],
                "stdout_tail": last[side]["stdout"][:200].rstrip(),
            }

        sp = stat("pyc")
        so = stat("oracle")
        if not sp["times"] or not so["times"]:
            record.update(status="ALL_TIMED_OUT", pyc=sp, oracle=so)
            self._log(f"  {BOLD}{w.name}{RST} {YEL}ALL_TIMED_OUT{RST}")
            return record
        speedup = so["median"] / sp["median"]
        mismatch = (sp["exit"] != so["exit"] or
                    last["pyc"]["stdout"] != last["oracle"]["stdout"])
        record.update(pyc=sp, oracle=so, speedup=round(speedup, 3),
                      mismatch=mismatch,
                      status="MISMATCH" if mismatch else "OK")

        art: dict[str, str] = {}
        if self.artifacts_dir is not None:
            ad = self.artifacts_dir / w.name
            ad.mkdir(parents=True, exist_ok=True)
            instr = self.build / f"{w.name}.instr"
            ci = self.compile(w.path, instr, ["--trace", "--flamegraph"])
            if ci["ok"]:
                te = dict(env)
                te["PYC_TRACE"] = str(ad / f"{w.name}.trace.ndjson")
                r = _timed_run([str(instr), *w.argv], cwd=cwd, env=te,
                               timeout=w.timeout)
                if not r["timed_out"] and (ad / f"{w.name}.trace.ndjson").exists():
                    art["trace"] = f"{w.name}/{w.name}.trace.ndjson"
                fe = dict(env)
                fe["PYC_FLAMEGRAPH"] = str(ad / f"{w.name}.flame.svg")
                r = _timed_run([str(instr), *w.argv], cwd=cwd, env=fe,
                               timeout=w.timeout)
                if not r["timed_out"] and (ad / f"{w.name}.flame.svg").exists():
                    art["flame"] = f"{w.name}/{w.name}.flame.svg"
            record["compile_instr_s"] = ci["elapsed"]
        record["artifacts"] = art

        flag = f" {RED}MISMATCH{RST}" if mismatch else ""
        self._log(f"  {BOLD}{w.name:<14}{RST} {w.cls:<12} "
                  f"{so['median']:>9.3f}s {sp['median']:>9.3f}s  "
                  f"{BOLD}{speedup:6.2f}x{RST}"
                  f" {DIM}p25-p75 {sp['p25']:.3f}-{sp['p75']:.3f}{RST}{flag}")
        return record

    def close(self) -> None:
        import shutil
        shutil.rmtree(self.build, ignore_errors=True)


def report(rec: dict) -> int:
    """The table. Every number is measured on this machine just now; nothing
    here is a prediction. Returns the process exit code."""
    n_bad = 0
    print()
    print(f"{BOLD}real-world performance{RST}  "
          f"{DIM}schema {SCHEMA}{RST}")
    print(f"  pyc    {rec['pyc']['binary']} {rec['pyc']['flags']}")
    print(f"  oracle {rec['oracle']['path']}  {rec['oracle']['version']}")
    print(f"  host   {rec['host']['class']}/{rec['host']['name']} "
          f"{DIM}git {rec['git']['sha'][:10] if rec['git']['sha'] else '?'}{RST}")
    print(f"  pin    {rec['pin_hash']}  set={rec['set']}  "
          f"runs={rec['runs']} warmup={rec['warmup']}")
    print()
    print(f"  {BOLD}{'workload':<15} {'class':<12} {'cpython':>10} "
          f"{'pyc':>10} {'speedup':>9}{RST}")
    for w in rec["workloads"]:
        if w["status"] != "OK":
            n_bad += 1
            print(f"  {BOLD}{w['name']:<15}{RST} {w['cls']:<12} "
                  f"{RED}{w['status']}{RST}  {DIM}{w.get('fail', w.get('compile_err', ''))[-160:] or ''}{RST}")
            continue
        sp, so = w["pyc"], w["oracle"]
        mark = f" {RED}MISMATCH{RST}" if w["mismatch"] else ""
        print(f"  {BOLD}{w['name']:<15}{RST} {w['cls']:<12} "
              f"{so['median']:>10.3f} {sp['median']:>10.3f} "
              f"{BOLD}{w['speedup']:>8.2f}x{RST}{mark}")
    if rec["artifacts_dir"]:
        print(f"\n  artifacts  {rec['artifacts_dir']}")
    print()
    if n_bad:
        print(f"  {RED}{n_bad} workload(s) flagged{RST}")
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", choices=sorted(perf_mod.SETS), default="smoke")
    ap.add_argument("--pyc", type=Path, default=None)
    ap.add_argument("--oracle", type=Path, default=None)
    ap.add_argument("--sysroot", type=Path, default=None)
    ap.add_argument("--pyc-flag", action="append", default=None,
                    help="compile flag for the timed build (repeatable; "
                         "default -O2). These enter the comparability key.")
    ap.add_argument("--runs", type=int, default=7,
                    help="timed runs per side (default 7; odd, so the median "
                         "is a real sample)")
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--compile-timeout", type=float, default=180.0)
    ap.add_argument("--artifacts-dir", type=Path, default=None,
                    help="where to write per-workload trace/flame artifacts "
                         "(default: a temp dir, printed at the end)")
    ap.add_argument("--no-artifacts", action="store_true",
                    help="skip the --trace/--flamegraph builds and runs")
    ap.add_argument("--json", type=Path, default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    # action="append" mutates a LIST default in place: with default=["-O2"],
    # a single --pyc-flag=-O2 produced ["-O2","-O2"] and the comparability
    # key drifted (caught on the first make-perf run). The default is
    # applied here instead, where it cannot be appended to.
    pyc_flags = tuple(args.pyc_flag) if args.pyc_flag else ("-O2",)

    pyc = args.pyc or find_pyc()
    if pyc is None:
        print("error: no pyc binary (--pyc or $PYC_BINARY)", file=sys.stderr)
        return 2
    pyc = pyc.resolve()
    if not pyc.is_file():
        print(f"error: --pyc is not an executable file: {pyc}", file=sys.stderr)
        return 2

    oracle = args.oracle
    if oracle is None:
        sysroot = args.sysroot or os.environ.get("PYC_SYSROOT")
        if sysroot:
            cand = Path(sysroot) / "bin" / "python3"
            if cand.exists():
                oracle = cand
    if oracle is None:
        # PATH python3 is NOT the oracle (CHARTER I5): the subject embeds the
        # sysroot CPython, so both sides must run the same interpreter set.
        print("error: no oracle (--oracle, --sysroot, or $PYC_SYSROOT); "
              "PATH python3 is not the sysroot (CHARTER I5)", file=sys.stderr)
        return 2
    oracle = Path(oracle).resolve()
    if not oracle.is_file():
        print(f"error: oracle not found: {oracle}", file=sys.stderr)
        return 2

    workloads = perf_mod.SETS[args.set]
    if args.runs < 1 or args.warmup < 0:
        print("error: --runs must be >= 1, --warmup >= 0", file=sys.stderr)
        return 2

    t_start = time.perf_counter()
    runner = Runner(pyc=pyc, oracle=oracle,
                    pyc_flags=pyc_flags,
                    runs=args.runs, warmup=args.warmup,
                    artifacts_dir=(None if args.no_artifacts
                                   else (args.artifacts_dir or
                                         Path(tempfile.mkdtemp(
                                             prefix="pyc-perf-art-")))),
                    compile_timeout=args.compile_timeout,
                    quiet=args.quiet)
    if not args.quiet:
        print(f"{BOLD}perf{RST} set={args.set} runs={args.runs} "
              f"warmup={args.warmup} pyc={' '.join(pyc_flags)}")
    rec_workloads = []
    for w in workloads:
        rec_workloads.append(runner.workload(w))
    runner.close()
    elapsed = time.perf_counter() - t_start

    rec = {
        "schema": SCHEMA,
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "git": git_identity(),
        "host": host_identity(),
        "pyc": {"binary": str(pyc), "flags": list(pyc_flags)},
        "oracle": oracle_identity(oracle),
        "set": args.set,
        "runs": args.runs,
        "warmup": args.warmup,
        "pin_hash": perf_mod.pin_hash(workloads),
        "elapsed_s": round(elapsed, 1),
        "artifacts_dir": str(runner.artifacts_dir) if not args.no_artifacts
        and runner.artifacts_dir else None,
        "workloads": rec_workloads,
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(rec, indent=2) + "\n")
        if not args.quiet:
            print(f"  record   {args.json}")
    return report(rec)


if __name__ == "__main__":
    sys.exit(main())
