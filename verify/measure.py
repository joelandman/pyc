"""Differential measurement: run a program twice under CPython and once under pyc.

This module MEASURES. It does not judge.

For each case it records six facts -- did it compile, what the oracle printed,
what the subject printed, and whether those differ on stdout, stderr and exit
status -- then attaches flags that follow mechanically from those facts. There
is no severity ranking, no verdict taxonomy and no quarantine, because each of
those is an inference, and an inference is not a measurement.

That distinction is not academic. The previous design ranked verdicts by
severity and used the ranking to detect regressions; because COMPILE_ERROR
ranked "better" than CRASH, fixing a compile error so a file finally RAN was
reported as 48 regressions. It also discarded cases it could not classify --
82 of 389 in one run -- throwing away measurements it had already taken. Both
faults were in the interpretation layer. Neither can occur here, because this
layer forms no opinions.

The oracle is run TWICE, before and after the subject, spanning compilation.
Those two runs must be identical. A program whose own output changes between
two runs of the same interpreter is not a usable measurement, and that is a
fact about the program or the environment -- reported as ORACLE_UNSTABLE, not
silently excluded.
"""

from __future__ import annotations

import dataclasses
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

SCHEMA = 4


# --------------------------------------------------------------------------
# Volatile text
#
# A value that CANNOT be the same twice is not evidence. Two runs of the same
# program at different times, under different load, at different heap layouts,
# must print a different elapsed time, a different clock reading and a
# different address -- so comparing those bytes measures the clock and the
# allocator, not the compiler.
#
# This is the one place where output is rewritten before comparison, and it is
# deliberately the narrowest thing that works:
#
#   * Both sides get the identical treatment. Nothing is applied to pyc that is
#     not applied to CPython.
#   * Each pattern matches one SHAPE. A value of that shape collapses to a
#     placeholder; anything else is untouched. pyc printing a malformed
#     timestamp, or an address where CPython prints a number, still differs.
#   * Raw output is what gets stored and shown. Normalisation decides only
#     whether a difference is reported.
#   * Every pattern that fires is named in the record, so a masked difference
#     can always be traced back to the rule that masked it.
#
# The cost is real and must be stated: a pyc bug that produces a WELL-FORMED
# wrong timestamp is invisible here. That is the price of not having the clock
# fail the build, and it is why nothing gets added to this list without a
# measured case that needs it.
#
# What is NOT normalised, though it also varies: pids, ephemeral ports, temp
# directory names, and anything from an unseeded `random`. Those stay visible
# as ORACLE_UNSTABLE -- they are the program choosing to print a coin flip, not
# an artefact of when the measurement happened to run.
# --------------------------------------------------------------------------

_MONTH = "Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec"
_DAY = "Mon|Tue|Wed|Thu|Fri|Sat|Sun"

VOLATILE: tuple[tuple[str, re.Pattern[str], str], ...] = (
    # unittest's trailer: "Ran 6 tests in 0.001s". The count is meaningful and
    # is left alone; only the duration goes.
    ("elapsed", re.compile(r"\bin \d+\.\d+s\b"), "in <ELAPSED>s"),
    # "<foo object at 0x7f3c605a4c20>", "<memory at 0x...>". Anchored on " at "
    # so a bare hex number in a program's output is not touched.
    ("heap_address", re.compile(r"(?<= at )0x[0-9a-fA-F]+"), "0xADDR"),
    # time.asctime / ctime: "Wed Aug 26 13:14:57 2026"
    ("asctime", re.compile(
        rf"\b(?:{_DAY}) (?:{_MONTH}) [ \d]\d \d{{2}}:\d{{2}}:\d{{2}} \d{{4}}\b"),
     "<ASCTIME>"),
    # ISO 8601, with or without fractional seconds.
    ("iso_datetime", re.compile(
        r"\b\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?\b"), "<ISOTIME>"),
    # Whatever clock text the first two did not claim.
    ("clock_time", re.compile(r"\b\d{2}:\d{2}:\d{2}\b"), "<TIME>"),
    ("iso_date", re.compile(r"\b\d{4}-\d{2}-\d{2}\b"), "<DATE>"),
)


def normalize(text: str) -> tuple[str, list[str]]:
    """Collapse values that cannot be equal twice. Returns the text and the
    names of the rules that actually fired, so the masking is auditable."""
    applied: list[str] = []
    for name, pattern, replacement in VOLATILE:
        text, n = pattern.subn(replacement, text)
        if n:
            applied.append(name)
    return text, applied


class Flag:
    """Mechanical consequences of the measurements. Not a severity scale.

    A case carries any number of these, or none. Nothing here ranks them; the
    only distinction drawn is IMPACTFUL, which says whether a difference
    changes what the program computed. A stderr difference is worth seeing --
    a traceback's wording, a warning -- but it does not change the answer, so
    it does not count against the pass rate.
    """

    DID_NOT_COMPILE = "DID_NOT_COMPILE"
    ORACLE_UNSTABLE = "ORACLE_UNSTABLE"
    TIMEOUT = "TIMEOUT"
    STDOUT_DIFFERS = "STDOUT_DIFFERS"
    EXIT_DIFFERS = "EXIT_DIFFERS"
    STDERR_DIFFERS = "STDERR_DIFFERS"

    IMPACTFUL = frozenset({DID_NOT_COMPILE, ORACLE_UNSTABLE, TIMEOUT,
                           STDOUT_DIFFERS, EXIT_DIFFERS})
    ALL = (DID_NOT_COMPILE, ORACLE_UNSTABLE, TIMEOUT,
           STDOUT_DIFFERS, EXIT_DIFFERS, STDERR_DIFFERS)


@dataclasses.dataclass
class Run:
    stdout: str = ""
    stderr: str = ""
    exit: int = 0
    timed_out: bool = False
    attempts: int = 1          # 2 when the first attempt timed out and it was retried
    timeout_used: float = 0.0  # the limit the FINAL attempt ran under

    @property
    def norm_stdout(self) -> str:
        return normalize(self.stdout)[0]

    @property
    def norm_stderr(self) -> str:
        return normalize(self.stderr)[0]

    @property
    def normalizers(self) -> list[str]:
        return sorted(set(normalize(self.stdout)[1]) | set(normalize(self.stderr)[1]))

    def same_as(self, other: "Run") -> bool:
        """Compared after normalisation, so two runs that differ only in when
        they happened are the same run."""
        return (self.norm_stdout == other.norm_stdout
                and self.norm_stderr == other.norm_stderr
                and self.exit == other.exit)


@dataclasses.dataclass
class Case:
    path: Path
    argv: tuple[str, ...] = ()
    stdin: str = ""
    name: str = ""

    def __post_init__(self) -> None:
        if not self.name:
            self.name = self.path.name + (
                " " + " ".join(self.argv) if self.argv else "")


@dataclasses.dataclass
class Measurement:
    """Everything observed about one case, plus the flags that follow from it."""

    case: Case
    compiled: bool = False
    diagnostic: str = ""            # compiler's first line when it refused
    oracle: Run | None = None
    oracle_again: Run | None = None
    subject: Run | None = None
    flags: list[str] = dataclasses.field(default_factory=list)

    @property
    def normalizers(self) -> list[str]:
        """Which volatile-text rules fired on this case, either side. Recorded
        so a masked difference can be traced to the rule that masked it."""
        out: set[str] = set()
        for r in (self.oracle, self.subject):
            if r is not None:
                out.update(r.normalizers)
        return sorted(out)

    @property
    def impactful(self) -> bool:
        return any(f in Flag.IMPACTFUL for f in self.flags)

    @property
    def clean(self) -> bool:
        return not self.flags

    def first_stdout_diff(self) -> str:
        """The first differing stdout line, as evidence rather than a summary."""
        if not (self.oracle and self.subject):
            return ""
        a = self.oracle.norm_stdout.splitlines()
        b = self.subject.norm_stdout.splitlines()
        for i in range(max(len(a), len(b))):
            x = a[i] if i < len(a) else "<no line>"
            y = b[i] if i < len(b) else "<no line>"
            if x != y:
                return f"line {i + 1}: cpython {x[:90]!r} pyc {y[:90]!r}"
        return ""


def _exec(cmd: list[str], *, cwd: Path, stdin: str, timeout: float,
          env: dict[str, str] | None = None, retries: int = 1) -> Run:
    """Run once; on a timeout, run again with DOUBLE the limit.

    A timeout is a statement about the machine as much as the program -- a
    loaded runner, a cold cache, a slow first import. Retrying at 2x separates
    "too slow for this limit right now" from "does not terminate", and only the
    second is worth reporting. The retry is the whole mechanism: if the doubled
    limit also expires, the run is a TIMEOUT and is reported as one, on
    whichever side it happened. It is never an oracle failure.
    """
    limit = timeout
    for attempt in range(1, retries + 2):
        try:
            p = subprocess.run(cmd, cwd=cwd, input=stdin, capture_output=True,
                               text=True, timeout=limit, env=env, errors="replace")
        except subprocess.TimeoutExpired:
            if attempt <= retries:
                limit *= 2
                continue
            return Run(timed_out=True, exit=-1, attempts=attempt, timeout_used=limit)
        except OSError as e:
            return Run(stderr=f"exec failed: {e}", exit=127, attempts=attempt,
                       timeout_used=limit)
        return Run(p.stdout, p.stderr, p.returncode, attempts=attempt,
                   timeout_used=limit)
    raise AssertionError("unreachable")


def child_env() -> dict[str, str]:
    """The environment BOTH sides run under.

    Pinning this is not normalising output: nothing the programs produce is
    rewritten. It fixes what they are run UNDER, so a difference is a
    difference in the programs rather than in the terminal. PYTHONHASHSEED
    affects set and dict iteration order on both sides, since the compiled
    binary embeds CPython too; the colour variables decide whether tracebacks
    carry ANSI escapes, which otherwise depends on the invoking shell.
    """
    env = dict(os.environ)
    env["PYTHONHASHSEED"] = "0"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHON_COLORS"] = "0"
    env["NO_COLOR"] = "1"
    env.pop("FORCE_COLOR", None)
    return env


class Compiler:
    """Isolates how pyc is invoked, so this survives changes to its CLI."""

    def __init__(self, binary: Path, flags: tuple[str, ...] = ()):
        self.binary = Path(binary)
        self.flags = flags

    def compile(self, src: Path, out: Path, *, cwd: Path, timeout: float) -> Run:
        return _exec([str(self.binary), str(src), "-o", str(out), *self.flags],
                     cwd=cwd, stdin="", timeout=timeout)


class Measurer:
    def __init__(self, *, oracle: Path, compiler: Compiler,
                 run_timeout: float = 30.0, compile_timeout: float = 180.0):
        self.oracle = Path(oracle)
        self.compiler = compiler
        self.run_timeout = run_timeout
        self.compile_timeout = compile_timeout

    def _run_oracle(self, case: Case, cwd: Path) -> Run:
        return _exec([str(self.oracle), str(case.path), *case.argv],
                     cwd=cwd, stdin=case.stdin, timeout=self.run_timeout,
                     env=child_env())

    def measure(self, case: Case) -> Measurement:
        m = Measurement(case=case)
        with tempfile.TemporaryDirectory(prefix="pyc-measure-") as td:
            cwd = Path(td)
            local = cwd / case.path.name
            shutil.copy2(case.path, local)
            # Siblings come too, so a program that imports the module next to it
            # resolves the same way it would in its own directory.
            for sib in case.path.parent.glob("*.py"):
                if sib != case.path and not (cwd / sib.name).exists():
                    shutil.copy2(sib, cwd / sib.name)
            local_case = dataclasses.replace(case, path=local)

            m.oracle = self._run_oracle(local_case, cwd)

            binary = cwd / (case.path.stem + ".pycbin")
            comp = self.compiler.compile(local, binary, cwd=cwd,
                                         timeout=self.compile_timeout)
            m.compiled = comp.exit == 0 and binary.exists() and not comp.timed_out
            if not m.compiled:
                m.diagnostic = _first_line(comp) or (
                    "compiler timed out" if comp.timed_out else
                    f"compiler exited {comp.exit}")
                m.flags.append(Flag.DID_NOT_COMPILE)
            else:
                m.subject = _exec([str(binary), *case.argv], cwd=cwd,
                                  stdin=case.stdin, timeout=self.run_timeout,
                                  env=child_env())

            # Second oracle run, AFTER the subject. The gap spans compilation,
            # which is where a program that reads the clock diverges from
            # itself; two adjacent runs would agree and hide it.
            m.oracle_again = self._run_oracle(local_case, cwd)

        if (not m.oracle.timed_out and not m.oracle_again.timed_out
                and not m.oracle.same_as(m.oracle_again)):
            # CPython disagreeing with CPython, AFTER normalisation -- so this
            # is no longer the clock or the allocator. What is left is the
            # program printing a pid, a port, a temp path or a coin flip. The
            # case cannot serve as a measurement until that is understood, and
            # saying so is more useful than dropping it.
            m.flags.append(Flag.ORACLE_UNSTABLE)

        # A timeout on EITHER side is a timeout, reported as one. Both sides
        # already got a second attempt at double the limit, so reaching here
        # means the program did not finish in 3x its budget.
        if m.oracle.timed_out or (m.subject is not None and m.subject.timed_out):
            m.flags.append(Flag.TIMEOUT)
            m.diagnostic = m.diagnostic or (
                "CPython did not finish within "
                f"{m.oracle.timeout_used:g}s" if m.oracle.timed_out else
                f"the binary did not finish within {m.subject.timeout_used:g}s")
        elif m.subject is not None:
            # Normalised on both sides: a value that cannot be equal twice is
            # not evidence of anything the compiler did.
            if m.subject.norm_stdout != m.oracle.norm_stdout:
                m.flags.append(Flag.STDOUT_DIFFERS)
            if m.subject.exit != m.oracle.exit:
                m.flags.append(Flag.EXIT_DIFFERS)
            if m.subject.norm_stderr != m.oracle.norm_stderr:
                m.flags.append(Flag.STDERR_DIFFERS)
        return m


def _first_line(r: Run, limit: int = 300) -> str:
    for line in (r.stderr or r.stdout).splitlines():
        if line.strip():
            return line.strip()[:limit]
    return ""
