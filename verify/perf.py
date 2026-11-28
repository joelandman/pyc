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
    # Extra files the workload depends on but does not itself carry -- the
    # vendored third-party source of a real-app workload. Folded into the set
    # pin (see pin_hash) so a change to that real-world code changes the
    # identity just like an input edit would.
    pinned_files: tuple[str, ...] = ()

    @property
    def relpath(self) -> str:
        try:
            return str(self.path.relative_to(REPO))
        except ValueError:
            return str(self.path)

    @property
    def content_sha(self) -> str:
        """Hash of the workload FILE, not just its name/path/argv. The
        pinned inputs mostly live in the file (N=500, ROWS=200000, ...), so
        a constant edit must change the pin: otherwise the baseline would
        silently keep comparing a workload that now measures a different
        amount of work."""
        return hashlib.sha256(self.path.read_bytes()).hexdigest()[:16]

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
    # Real-app: unmodified third-party pure-Python code, vendored under
    # verify/corpus/realapp/ and pinned via pinned_files. Two different code
    # paths and call shapes than any named workload -- the class STATUS.md
    # was missing. json_validate exercises fastjsonschema (schema validation,
    # including the validator it generates at runtime); host_encode exercises
    # idna (IDNA/punycode encoding). Sized for ~1.5 s of work on the oracle.
    Workload("json_validate", REAL_APP, WORKLOADS_DIR / "json_validate.py",
             pinned_files=("verify/corpus/realapp/fastjsonschema",)),
    Workload("host_encode", REAL_APP, WORKLOADS_DIR / "host_encode.py",
             pinned_files=("verify/corpus/realapp/idna",)),
]

SETS: dict[str, list[Workload]] = {"smoke": SMOKE, "full": FULL}


def pin_hash(workloads: list[Workload]) -> str:
    """The identity of a workload set: name + relative path + argv + file
    content, per workload. A change to any pinned input (a constant in the
    file, an argv, a path) changes the hash, and perf_compare then refuses
    to compare -- the same refusal measure_compare gives you for a flag or
    version drift. The per-workload form of this pin is what the comparator
    actually enforces: a set can be compared against a baseline recorded
    for a DIFFERENT set (the nightly full set against the smoke baseline)
    as long as every workload in the intersection is pin-identical."""
    parts = [f"{w.name}|{w.relpath}|{','.join(w.argv)}|{w.content_sha}"
             for w in workloads]
    # A real-app workload measures vendored third-party source that lives
    # outside its own file; fold that source into the pin too, so a change to
    # the real-world code changes the set identity just like an input edit
    # would (perf_compare refuses a set whose per-workload pins drift).
    for w in workloads:
        for rel in w.pinned_files:
            base = REPO / rel
            targets = sorted(base.rglob("*.py")) if base.is_dir() else [base]
            for fp in targets:
                tag = fp.relative_to(REPO).as_posix()
                parts.append(
                    f"{w.name}:{tag}|{hashlib.sha256(fp.read_bytes()).hexdigest()[:16]}")
    return hashlib.sha256("\n".join(parts).encode()).hexdigest()[:16]
