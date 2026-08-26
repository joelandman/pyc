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
import shutil
import subprocess
import tempfile
from pathlib import Path

SCHEMA = 3


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

    def same_as(self, other: "Run") -> bool:
        return (self.stdout == other.stdout and self.stderr == other.stderr
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
    def impactful(self) -> bool:
        return any(f in Flag.IMPACTFUL for f in self.flags)

    @property
    def clean(self) -> bool:
        return not self.flags

    def first_stdout_diff(self) -> str:
        """The first differing stdout line, as evidence rather than a summary."""
        if not (self.oracle and self.subject):
            return ""
        a, b = self.oracle.stdout.splitlines(), self.subject.stdout.splitlines()
        for i in range(max(len(a), len(b))):
            x = a[i] if i < len(a) else "<no line>"
            y = b[i] if i < len(b) else "<no line>"
            if x != y:
                return f"line {i + 1}: cpython {x[:90]!r} pyc {y[:90]!r}"
        return ""


def _exec(cmd: list[str], *, cwd: Path, stdin: str, timeout: float,
          env: dict[str, str] | None = None) -> Run:
    try:
        p = subprocess.run(cmd, cwd=cwd, input=stdin, capture_output=True,
                           text=True, timeout=timeout, env=env, errors="replace")
    except subprocess.TimeoutExpired:
        return Run(timed_out=True, exit=-1)
    except OSError as e:
        return Run(stderr=f"exec failed: {e}", exit=127)
    return Run(p.stdout, p.stderr, p.returncode)


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

        if not m.oracle.same_as(m.oracle_again):
            # CPython disagreeing with CPython. The case cannot serve as a
            # measurement until that is understood, and saying so is more
            # useful than dropping it.
            m.flags.append(Flag.ORACLE_UNSTABLE)

        if m.subject is not None:
            if m.subject.timed_out:
                m.flags.append(Flag.TIMEOUT)
            else:
                if m.subject.stdout != m.oracle.stdout:
                    m.flags.append(Flag.STDOUT_DIFFERS)
                if m.subject.exit != m.oracle.exit:
                    m.flags.append(Flag.EXIT_DIFFERS)
                if m.subject.stderr != m.oracle.stderr:
                    m.flags.append(Flag.STDERR_DIFFERS)
        return m


def _first_line(r: Run, limit: int = 300) -> str:
    for line in (r.stderr or r.stdout).splitlines():
        if line.strip():
            return line.strip()[:limit]
    return ""
