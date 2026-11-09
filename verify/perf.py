"""Real-world performance workloads (the set in STATUS.md's
'Real-world performance' section).

CHARTER I5 discipline carries over: every workload is a real program
with a deterministic input -- pinned constants in the source, or inputs
built in memory from fixed patterns. No workload depends on the clock,
randomness without a seed, or network. A workload that is not
deterministic is not a perf workload here: its number is a coin-flip,
and a coin-flip in a trend line is how a compiler earns a wrong
reputation (I9).

A workload never carries an expected output in this harness. The
harness measures time and exit status; it does not judge the program
(correctness is the differential harness's job, and these same
programs run there). Each program prints a small checksum-of-work so a
silently-empty run is visible in the record.
"""
from __future__ import annotations

import dataclasses
import hashlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKLOADS_DIR = Path(__file__).resolve().parent / "perf" / "workloads"
CORPROG = Path(__file__).resolve().parent / "corpus" / "programs"

# Class names, as used in STATUS.md's table:
#   pure-python  -- call/combinatorial; native dispatch should win
#   numeric      -- type-stable float/int; the unboxing target (CHARTER §4)
#   c-bound      -- C work dominates; the honest floor
#   real-app     -- unmodified third-party code (nightly set)
PURE_PYTHON = "pure-python"
NUMERIC = "numeric"
C_BOUND = "c-bound"
REAL_APP = "real-app"

@dataclasses.dataclass(frozen=True)
class Workload:
    name: str
    cls: str
    path: Path
    argv: tuple[str, ...] = ()
    timeout: float = 180.0

    @property
    def relpath(self) -> str:
        try:
            return str(self.path.relative_to(REPO))
        except ValueError:
            return str(self.path)

SMOKE: list[Workload] = [
    # Small, stable, fast to compile: the PR-gate trend set. Each is sized
    # for ~1-2 s of work on the oracle, so process startup is a small,
    # symmetric fraction of the measurement rather than the measurement.
    Workload("nbody", NUMERIC, CORPROG / "nbody.py", ("500000",), 120.0),
    Workload("spectralnorm", NUMERIC, WORKLOADS_DIR / "spectralnorm.py"),
    Workload("ast_parse", PURE_PYTHON, WORKLOADS_DIR / "ast_parse.py"),
    Workload("xml_build", PURE_PYTHON, WORKLOADS_DIR / "xml_build.py"),
    Workload("decimal_pi", C_BOUND, WORKLOADS_DIR / "decimal_pi.py"),
    Workload("nqueens", PURE_PYTHON, WORKLOADS_DIR / "nqueens.py"),
]

FULL: list[Workload] = SMOKE + [
    # The nightly set: adds the slower numeric workloads.
    Workload("pidigits", NUMERIC, WORKLOADS_DIR / "pidigits.py"),
    Workload("matmul", NUMERIC, WORKLOADS_DIR / "matmul.py"),
    Workload("mail_parse", PURE_PYTHON, WORKLOADS_DIR / "mail_parse.py"),
    Workload("mandelbrot", NUMERIC, CORPROG / "mandelbrot.py", ("1000",)),
]

SETS: dict[str, list[Workload]] = {"smoke": SMOKE, "full": FULL}


def pin_hash(workloads: list[Workload]) -> str:
    """The comparability key for the workload set: name + relative path +
    argv. A change to any pinned input (a constant, an argv, a path)
    changes the hash, and perf_compare then refuses to compare -- the
    same refusal measure_compare gives you for a flag or version drift."""
    parts = [f"{w.name}|{w.relpath}|{','.join(w.argv)}" for w in workloads]
    return hashlib.sha256("\n".join(parts).encode()).hexdigest()[:16]
