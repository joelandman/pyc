"""Differential execution: run a program under pyc and under CPython, compare.

CHARTER I5 — no test may hardcode expected output. The oracle is the pinned
CPython binary, at run time. There is deliberately nowhere in this module to
put an expected value.

CHARTER I1 — a silent wrong answer is the worst outcome available. The
classification below ranks it above crashes, and nothing in the reporting path
may reorder that.
"""

from __future__ import annotations

import dataclasses
import enum
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


class Verdict(enum.Enum):
    """Ordered worst-first. The integer is the priority rank, not a severity
    score — P0 is the thing that must be fixed before anything else."""

    # P0 — pyc claimed success and lied.
    SILENT_WRONG_ANSWER = 0
    # P1 — pyc failed loudly. Bad, but honest (I1 permits this).
    CRASH = 1
    HANG = 2
    # P2 — pyc refused to compile. This is the CORRECT response to an
    # unsupported construct, and is how coverage gaps should present.
    COMPILE_ERROR = 3
    # P3 — right answer, wrong diagnostics.
    STDERR_DIFF = 4
    # Not a finding.
    MATCH = 5
    # Excluded from scoring; the oracle could not establish a ground truth.
    QUARANTINE_NONDETERMINISTIC = 6
    QUARANTINE_ORACLE_FAILED = 7

    @property
    def priority(self) -> str:
        return {
            Verdict.SILENT_WRONG_ANSWER: "P0",
            Verdict.CRASH: "P1",
            Verdict.HANG: "P1",
            Verdict.COMPILE_ERROR: "P2",
            Verdict.STDERR_DIFF: "P3",
        }.get(self, "-")

    @property
    def is_finding(self) -> bool:
        return self.value <= Verdict.STDERR_DIFF.value

    @property
    def is_scored(self) -> bool:
        """Quarantined cases are excluded from the completeness metric."""
        return self.value <= Verdict.MATCH.value


@dataclasses.dataclass
class Execution:
    stdout: str
    stderr: str
    returncode: int
    timed_out: bool = False
    signal: int | None = None

    @property
    def crashed_by_signal(self) -> bool:
        return self.returncode < 0


@dataclasses.dataclass
class Case:
    """One program to compare. `path` is the source; argv/stdin let a corpus
    exercise the same source under different inputs."""

    path: Path
    argv: tuple[str, ...] = ()
    stdin: str = ""
    name: str = ""

    def __post_init__(self) -> None:
        if not self.name:
            self.name = self.path.name
            if self.argv:
                self.name += " " + " ".join(self.argv)


@dataclasses.dataclass
class Result:
    case: Case
    verdict: Verdict
    oracle: Execution | None = None
    subject: Execution | None = None
    detail: str = ""

    def diff_block(self, limit: int = 2000) -> str:
        """Human-readable divergence. Never used to *decide* anything — only
        to explain a decision already made."""
        if self.oracle is None or self.subject is None:
            return self.detail
        import difflib

        out = [self.detail] if self.detail else []
        for label, a, b in (
            ("stdout", self.oracle.stdout, self.subject.stdout),
            ("stderr", self.oracle.stderr, self.subject.stderr),
        ):
            if a != b:
                d = "\n".join(
                    difflib.unified_diff(
                        a.splitlines(), b.splitlines(),
                        fromfile=f"cpython/{label}", tofile=f"pyc/{label}",
                        lineterm="",
                    )
                )
                out.append(d[:limit] + ("\n… (truncated)" if len(d) > limit else ""))
        if self.oracle.returncode != self.subject.returncode:
            out.append(
                f"exit: cpython={self.oracle.returncode} pyc={self.subject.returncode}"
            )
        return "\n".join(out)


def _child_env() -> dict[str, str]:
    """The environment BOTH sides run under.

    The oracle used to get a pinned environment while the compiled binary
    inherited whatever the shell had. That is wrong twice over:

    * The binary embeds CPython, so PYTHONHASHSEED affects its set and dict
      iteration order too. Pinning it for only one side leaves a real source of
      divergence in the comparison itself.
    * Traceback colouring is decided by the environment (FORCE_COLOR,
      NO_COLOR, PYTHON_COLORS, whether stderr is a tty). With FORCE_COLOR set
      in a developer's shell the oracle emitted ANSI escapes and the subject
      did not, so a stderr verdict depended on the terminal the harness was
      invoked from and did not reproduce in CI.

    Pinning the environment is not normalising the output: nothing about the
    programs' behaviour is rewritten. It fixes what they are run UNDER, so the
    comparison measures the program rather than the terminal.
    """
    env = dict(os.environ)
    # Hash randomization makes set/dict-of-str iteration order vary between
    # runs. Pinning it does not paper over a divergence -- pyc is compared
    # against whatever order CPython then produces.
    env["PYTHONHASHSEED"] = "0"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHON_COLORS"] = "0"
    env.pop("FORCE_COLOR", None)
    env["NO_COLOR"] = "1"
    return env


def _run(cmd: list[str], *, cwd: Path, stdin: str, timeout: float,
         env: dict[str, str] | None = None) -> Execution:
    try:
        p = subprocess.run(
            cmd, cwd=cwd, input=stdin, capture_output=True, text=True,
            timeout=timeout, env=env, errors="replace",
        )
    except subprocess.TimeoutExpired:
        return Execution("", "", -1, timed_out=True)
    except OSError as e:
        return Execution("", f"exec failed: {e}", 127)
    return Execution(p.stdout, p.stderr, p.returncode)


class CompilerAdapter:
    """Isolates how pyc is invoked, so the harness survives the rebuild.

    The new tree may change flags entirely; only this class should need to
    change with it.
    """

    def __init__(self, binary: Path, extra_flags: tuple[str, ...] = ()):
        self.binary = Path(binary)
        self.extra_flags = extra_flags

    def compile(self, src: Path, out: Path, *, cwd: Path,
                timeout: float) -> Execution:
        cmd = [str(self.binary), str(src), "-o", str(out), *self.extra_flags]
        return _run(cmd, cwd=cwd, stdin="", timeout=timeout)


class DifferentialRunner:
    def __init__(
        self,
        *,
        oracle: Path,
        compiler: CompilerAdapter,
        run_timeout: float = 30.0,
        compile_timeout: float = 180.0,
        nondeterminism_probe: bool = True,
        stability_samples: int = 3,
    ):
        self.oracle = Path(oracle)
        self.compiler = compiler
        self.run_timeout = run_timeout
        self.compile_timeout = compile_timeout
        self.nondeterminism_probe = nondeterminism_probe
        self.stability_samples = stability_samples

    # -- oracle -------------------------------------------------------------

    def _run_oracle(self, case: Case, cwd: Path) -> Execution:
        return _run(
            [str(self.oracle), str(case.path), *case.argv],
            cwd=cwd, stdin=case.stdin, timeout=self.run_timeout,
            env=_child_env(),
        )

    def _is_nondeterministic(self, case: Case, cwd: Path,
                             first: Execution) -> bool:
        """Establish ground truth *empirically* rather than by writing
        normalization rules.

        Hand-written normalizers (strip hex addresses, strip timings) are a
        standing invitation to mask a real divergence. Running the oracle twice
        and quarantining anything unstable cannot mask anything: a case either
        has a reproducible ground truth or it is excluded from scoring.
        """
        if not self.nondeterminism_probe:
            return False
        second = self._run_oracle(case, cwd)
        return (
            second.stdout != first.stdout
            or second.returncode != first.returncode
        )

    # -- main ---------------------------------------------------------------

    def run(self, case: Case) -> Result:
        with tempfile.TemporaryDirectory(prefix="pyc-verify-") as td:
            cwd = Path(td)
            # Copy the source in so relative imports/data resolve, and so the
            # compiler cannot write artifacts into the repo.
            local_src = cwd / case.path.name
            shutil.copy2(case.path, local_src)
            for sib in case.path.parent.glob("*.py"):
                if sib != case.path and not (cwd / sib.name).exists():
                    shutil.copy2(sib, cwd / sib.name)
            local_case = dataclasses.replace(case, path=local_src)

            oracle = self._run_oracle(local_case, cwd)
            if oracle.timed_out:
                return Result(case, Verdict.QUARANTINE_ORACLE_FAILED, oracle, None,
                              "CPython itself timed out")
            if self._is_nondeterministic(local_case, cwd, oracle):
                return Result(case, Verdict.QUARANTINE_NONDETERMINISTIC, oracle,
                              None, "CPython output not reproducible across runs")

            binary = cwd / (case.path.stem + ".pycbin")
            comp = self.compiler.compile(local_src, binary, cwd=cwd,
                                         timeout=self.compile_timeout)
            if comp.timed_out:
                return Result(case, Verdict.COMPILE_ERROR, oracle, comp,
                              "compiler timed out")
            if comp.returncode != 0 or not binary.exists():
                return Result(case, Verdict.COMPILE_ERROR, oracle, comp,
                              _first_diagnostic(comp))

            subject = _run([str(binary), *case.argv], cwd=cwd,
                           stdin=case.stdin, timeout=self.run_timeout,
                           env=_child_env())
            provisional = self._classify(case, oracle, subject)
            # Re-run the ORACLE after the subject, spanning the real window.
            #
            # The up-front probe runs the oracle twice back to back, which
            # catches fast-varying output but not output that varies with the
            # WALL CLOCK: compilation sits between the oracle and the subject,
            # so a case printing the current time diverges across that gap while
            # two adjacent runs agree. Lib/test/test_strftime.py prints
            # `strftime test for <current time>` on line 1 and surfaced as a P0
            # silent wrong answer for exactly this reason.
            #
            # Only when a finding is about to be reported: re-running every
            # passing case would cost a full extra oracle pass for no signal.
            if (self.nondeterminism_probe and provisional.verdict.is_finding
                    and not subject.timed_out):
                later = self._run_oracle(local_case, cwd)
                if (later.stdout != oracle.stdout
                        or later.returncode != oracle.returncode):
                    return Result(case, Verdict.QUARANTINE_NONDETERMINISTIC,
                                  oracle, subject,
                                  "output varies with wall-clock time; the "
                                  "oracle changed across the compile window")

                # The SUBJECT can be unstable while the oracle is not. unittest
                # prints "Ran N tests in 0.001s", and under parallel load the
                # compiled binary drifts across that millisecond boundary while
                # CPython, run at a different moment, does not. Seventeen files
                # flipped MATCH -> STDERR_DIFF between two runs of the SAME
                # binary purely from `--jobs 12` scheduling.
                #
                # Re-run the subject and use its own variance to identify which
                # LINES are unstable. Quarantine only when every line that
                # differs from the oracle is one the subject cannot reproduce
                # itself -- so a real divergence sitting alongside a drifting
                # timer is still reported, which a blanket quarantine would
                # have swallowed.
                # Two samples cannot reliably detect a PROBABILISTIC drift:
                # measured on eight known-flapping files, one extra run still
                # left MATCH varying 3-4 across repeats. Sampling three times
                # and unioning the instability converges it. The cost is bounded
                # -- this runs only when a finding is about to be reported, not
                # per case.
                samples = [_run([str(binary), *case.argv], cwd=cwd,
                                stdin=case.stdin, timeout=self.run_timeout,
                                env=_child_env())
                           for _ in range(self.stability_samples)]
                again = samples[0]
                if all(x.returncode == subject.returncode for x in samples):
                    # stdout and stderr are kept SEPARATE: merging the index
                    # sets would let an unstable stdout line 3 excuse a real
                    # stderr difference on line 3.
                    out_unstable, err_unstable = set(), set()
                    for x in samples:
                        out_unstable |= _unstable_lines(subject.stdout, x.stdout)
                        err_unstable |= _unstable_lines(subject.stderr, x.stderr)
                    if out_unstable or err_unstable:
                        out_diff = _diff_lines(oracle.stdout, subject.stdout)
                        err_diff = _diff_lines(oracle.stderr, subject.stderr)
                        if (out_diff <= out_unstable
                                and err_diff <= err_unstable):
                            return Result(
                                case, Verdict.QUARANTINE_NONDETERMINISTIC,
                                oracle, subject,
                                "every differing line is one the compiled "
                                "program does not reproduce across runs")
            return provisional

    def _classify(self, case: Case, oracle: Execution,
                  subject: Execution) -> Result:
        def r(v: Verdict, detail: str = "") -> Result:
            return Result(case, v, oracle, subject, detail)

        if subject.timed_out:
            return r(Verdict.HANG, f"pyc exceeded {self.run_timeout}s; CPython did not")

        stdout_match = subject.stdout == oracle.stdout
        exit_match = subject.returncode == oracle.returncode

        # I1: the defining case. pyc claimed success and produced something
        # other than Python's answer. Ranked above every crash.
        if subject.returncode == 0 and not (stdout_match and oracle.returncode == 0):
            if not stdout_match:
                return r(Verdict.SILENT_WRONG_ANSWER, "pyc exited 0 with wrong stdout")
            return r(Verdict.SILENT_WRONG_ANSWER,
                     f"pyc exited 0; CPython exited {oracle.returncode}")

        if subject.crashed_by_signal:
            return r(Verdict.CRASH, f"killed by signal {-subject.returncode}")
        if not stdout_match or not exit_match:
            return r(Verdict.CRASH, _failure_reason(subject, oracle))
        if subject.stderr != oracle.stderr:
            return r(Verdict.STDERR_DIFF, "stdout and exit match; stderr differs")
        return r(Verdict.MATCH)


def _unstable_lines(a: str, b: str) -> set[int]:
    """Line indices that differ between two runs of the SAME program.

    Measured instability, not a pattern. The alternative -- a rule that strips
    anything looking like a duration -- is exactly the normalisation this
    harness refuses, because such a rule cannot tell a drifting timer from a
    genuinely wrong number.
    """
    la, lb = a.splitlines(), b.splitlines()
    out = {i for i in range(min(len(la), len(lb))) if la[i] != lb[i]}
    out.update(range(min(len(la), len(lb)), max(len(la), len(lb))))
    return out


def _diff_lines(a: str, b: str) -> set[int]:
    """Line indices where two DIFFERENT programs' outputs differ."""
    return _unstable_lines(a, b)


def _last_line(text: str) -> str:
    for line in reversed(text.splitlines()):
        if line.strip():
            return line.strip()
    return ""


def _failure_reason(subject: Execution, oracle: Execution,
                    limit: int = 200) -> str:
    """Why pyc failed, in a form that GROUPS across cases.

    This used to be the fixed string "pyc failed, but stdout/exit differ from
    CPython", which made CRASH -- the largest failure category against
    Lib/test, 165 of 389 files -- completely unrankable. COMPILE_ERROR records
    its first diagnostic and ranks cleanly; this did not, so half the failures
    were invisible while the other half were a to-do list.

    The last stderr line is the useful one: for a Python-level failure that is
    `ExceptionType: message`, which is exactly the grouping key. It is only
    reported when it DIFFERS from CPython's -- if both ended the same way the
    divergence is in stdout or exit status, and quoting a shared traceback
    would be actively misleading.
    """
    sub = _last_line(subject.stderr)
    ora = _last_line(oracle.stderr)
    if sub and sub != ora:
        return sub[:limit]
    if subject.returncode != oracle.returncode:
        return (f"exit {subject.returncode} vs CPython {oracle.returncode}"
                + (f"; {sub[:limit]}" if sub else ""))
    return "stdout differs; stderr and exit match CPython"


def _first_diagnostic(e: Execution, limit: int = 300) -> str:
    for line in (e.stderr or e.stdout).splitlines():
        s = line.strip()
        if s:
            return s[:limit]
    return f"compiler exited {e.returncode} with no diagnostic"
