# verify — differential harness (agent A5)

Owns the correctness bar. Reports to the user, not to the feature agents;
its results may not be overridden by them (`rebuild/AGENT_DIRECTIVES.md`, A5).

## The one rule

**No test in this tree stores an expected output.** The oracle is a real
CPython binary, executed at run time, every run. There is deliberately nowhere
in `differential.py` to put an expected value.

This is CHARTER I5, and it exists because of how the old tree drifted: 662
inline `(source, expected)` pairs, with the expected string documented as the
source of truth. Such a suite measures "does the curated subset still work" and
stays green forever while the gap to real Python never narrows.

## Usage

```bash
./verify/run.py --corpus tests/                 # a directory of programs
./verify/run.py --libtest                       # CPython Lib/test — the metric
./verify/run.py --file prog.py --show 4         # one program, with diffs
./verify/run.py --corpus tests/ --fail-on P0    # CI gate
./verify/run.py --corpus tests/ --json out.json # machine-readable
```

The subject binary comes from `--pyc`, `$PYC_BINARY`, or `./build/pyc`. The
oracle comes from `--oracle`, or `--sysroot DIR` (read from that sysroot's
`pyc-sysroot.json`), or the running interpreter. Prefer `--sysroot`: a
divergence must be measured against the runtime actually being targeted, and
that makes the parse oracle and the differential oracle the same binary
(`rebuild/VERSION_TARGETING.md`).

## Verdicts, worst first

| | Verdict | Meaning |
|---|---|---|
| **P0** | `SILENT_WRONG_ANSWER` | pyc exited 0 and produced something other than Python's answer |
| P1 | `CRASH` | pyc failed loudly — bad, but honest |
| P1 | `HANG` | pyc exceeded the timeout; CPython did not |
| P2 | `COMPILE_ERROR` | pyc refused to compile |
| P3 | `STDERR_DIFF` | right answer, wrong diagnostics |
| — | `MATCH` | |
| — | `QUARANTINE_*` | no reproducible ground truth; excluded from scoring |

**P0 outranks every crash.** That ordering is CHARTER I1 and is not a
severity heuristic: a compiler that crashes is a compiler you can trust to
tell you when it failed. One that returns 5 as `None`, or `math.factorial(25)`
as a wrapped int64, silently corrupts every result downstream. Nothing in the
reporting path may reorder this.

**P2 is not a bug.** A compile error is the *correct* response to an
unsupported construct (I1: fail loudly, never silently). Coverage gaps should
present as P2 and get resolved by implementing the feature — never by
downgrading the check.

## Gate outcomes (`check_regression.py`)

| Exit | Meaning | |
|---|---|---|
| 0 | gate ran, nothing regressed | a verdict |
| 1 | gate ran, something regressed | a verdict |
| 2 | **gate did not run** — not comparable | *not* a verdict |
| 3 | usage error | |

Exit 2 is not a failing gate; it means no comparison happened and the metric is
**unguarded**. The distinction is easy to lose because GitHub renders every
nonzero exit as the same red X — five consecutive `verify` runs exited 2 (the
`-O0` flag was missing from the workflow, so an `-O0` baseline was being
compared against a default-flags run) and read exactly like five failing gates.
Nobody looked, because a red X on a gate looks like a known-failing gate.

So a "did not run" outcome now announces itself three ways: a banner in the log,
a GitHub workflow annotation on the run page, and a block in the step summary.

`--require-baseline` closes the more dangerous version of the same hole. A
missing baseline used to print a note and return **0** — a green run in which
the gate protected nothing. `metric.yml` gated a baseline path that did not
exist and passed every night on that basis. CI always passes
`--require-baseline` so this fails loudly instead; green is worse than red here,
because nobody investigates green.

## Baseline staleness

The gate is one-directional by design: it blocks regressions and never adopts
improvements, because auto-adopting would let a bad run quietly become the new
reference. The cost is that a baseline drifts stale in the *good* direction,
silently, while every run keeps passing — the language baseline sat 15 cases
behind before anyone noticed, and only because someone compared by hand.

So a run that is better than its baseline prints **BASELINE IS STALE**, with a
`::warning` annotation and a step-summary block naming the refresh command. It
never fails: blocking a change for being better would be absurd.

Staleness is suppressed when the run also regressed. A real failure must not be
softened by good news sitting next to it.

Refresh deliberately, after checking what moved:

```bash
./verify/check_regression.py --update --baseline <baseline> --current <run>
```

## How nondeterminism is handled

Not with normalization rules. Hand-written normalizers — strip hex addresses,
strip timings, sort set output — are a standing invitation to mask a real
divergence, and you find out years later that the masking rule was hiding a
bug.

Instead the harness **runs the oracle twice** and quarantines any case whose
own output is not reproducible. A case either has a stable ground truth or it
is excluded from scoring. This cannot hide a divergence, because it never
edits output before comparing it.

`PYTHONHASHSEED=0` is pinned for the oracle so str-keyed set/dict iteration
order is stable between runs. That is not normalization: pyc is still compared
against whatever order CPython actually produces.

## Reading a result

**A pass rate is not reproducible without its measurement conditions.** Measured
on `Lib/test`, the *same binary* scored 51 matches at `--jobs 4` and 37 at
`--jobs 12` — 31 verdict changes from parallelism alone, because unittest prints
`Ran N tests in 0.001s` and that line drifts under load. The noise floor at
`--jobs 12` is roughly ±14 files, about 3.6 percentage points.

So, before reporting any delta as real:

1. **Control for it.** Re-run the *previous* binary, or the same binary under
   the changed condition. A delta inside the noise floor is not evidence.
2. **Check the mechanism is plausible.** A change to closure-cell ordering
   produces wrong values — a stdout difference. It cannot produce a stderr-only
   timing difference. If the observed failure shape does not match what the
   change could physically cause, suspect the measurement first.
3. **Prefer the corpus for correctness claims.** `verify/corpus/language` has no
   unittest timing text and has been stable at 99.04% across dozens of runs; it
   is the reliable signal for "did this break something".

`run.py` records `jobs` in its output and `check_regression.py` says so when
comparing across values, so a number cannot silently be compared against one
taken under different conditions.

This is written down because it was learned the expensive way: a 13.11% ->
9.51% "regression" was reported here as real, and a control run at `--jobs 4`
reproduced 13.11% exactly from the same binary. The reported drop was entirely
measurement noise.

## The metric (CHARTER I6)

**The published number is `matched / len(corpus)`** — of the 389 `Lib/test`
files, how many behave exactly as CPython does. The denominator is the corpus,
which is fixed, so the number moves only when the compiler does.

`matched / scored` is reported too, and is the more flattering figure, but it is
*not* the headline and does not gate. Its denominator is the scored set, which
excludes quarantined cases, and quarantine membership is nondeterministic. That
makes it move for reasons unrelated to the compiler — and backwards: a run with
less flakiness scores more cases and therefore reports a *lower* rate. The first
scheduled metric run failed on exactly this, reporting a regression from 1.96%
to 1.94% while matching the identical 7 cases.


`--libtest` scores against CPython's own `Lib/test/`. It is the only number
that cannot be gamed by adding more snippets, and it is published low and
honest from day one.

`corpus.py` skips some `Lib/test` files — C-API internals, tests that build
extensions or re-exec the interpreter, GUI toolkits. Each exclusion carries a
reason in the source, because the skip list is a statement about what the
metric measures. Grow the corpus; never quietly shrink it.

Note what running these files directly does and does not prove: many are
`unittest` modules with no `main` block, so both sides produce empty output and
the case reduces to an import test. That is still meaningful — it proves the
module-level code (imports, class bodies, decorators, annotations) compiles and
runs. Driving them under a unittest runner for deeper execution is a later
increment, and will lower the score when it lands. That is correct and
expected; do not treat the drop as a regression.

## Extending

- New corpus source → a generator of `Case` in `corpus.py`. It must not know
  an expected output; if it needs one, the design is wrong.
- New compiler CLI → `CompilerAdapter` in `differential.py` is the only place
  that knows how pyc is invoked, so the harness survives the rebuild.
- Property-based / generated programs are welcome. They fit the model exactly:
  the oracle supplies the ground truth, so generated input costs nothing extra.

## Baseline (2026-08-22)

Measured against the **old tree**'s `build/pyc` at `352ed91`, oracle CPython
3.14.7. This is the number the rebuild has to beat, recorded now so progress is
measurable from the first commit rather than asserted.

| Corpus | Pass rate |
|---|---|
| CPython `Lib/test/`, 60-file sample | **0.0%** (0/60 files) |

Breakdown: 30 `COMPILE_ERROR`, 11 `CRASH`, 8 `STDERR_DIFF`, **3 P0
`SILENT_WRONG_ANSWER`**.

The P0s are the finding worth reading. In `Lib/test/test_atexit.py`, pyc emits
six `ImportError:` lines to stderr and then **exits 0** — it imported nothing,
ran nothing, and reported success. An `ImportError` that does not fail the
process is the silent-wrong-answer class in its purest form, and no amount of
snippet testing would surface it.

0.0% is the correct starting number and should be published as-is (I6).

## The two baselines, and why the gap is the whole point

Same compiler (`build/pyc` at `4fdd2cf`), same harness, same oracle, same day:

| Corpus | Pass rate |
|---|---|
| The old tree's own rescued tests (806 programs) | **97.76%** (786/804) |
| CPython `Lib/test/`, 60-file sample | **0.0%** (0/52) |

That gap is the review's thesis expressed as a number. A suite built alongside
a compiler measures the subset that compiler was built for, and reports near
100% forever. It cannot see the distance to real Python, which is why I6 makes
`Lib/test` — a corpus nobody here wrote — the published metric.

### The hardcoded expectations were certifying pyc's bugs

Worse than uninformative. Three examples, all of which the old runner reported
as **passing**:

`case_273` / `case_274` use `cmp_to_key(...)` without importing it. CPython
raises `NameError`. The old runner's recorded expectation is
`'[1, 1, 3, 4, 5]\n'` — which is what pyc prints, because pyc exposes
`cmp_to_key` as a magic builtin. Verified: pyc prints exactly that and exits 0.

`case_525` calls `hashlib.md5("hello world")` on a `str`. CPython raises
`TypeError` (it requires bytes). The recorded expectation is the digest pyc
produces.

The expectations were evidently captured from pyc's own output, so the suite
certified the compiler's deviations as correct and locked them in. This is not
a hypothetical failure mode of hardcoded expectations — it is what happened
here, and it is the concrete reason CHARTER I5 forbids them.

The harness flags all five as **P0**: pyc accepts programs Python rejects and
silently produces answers Python never would.
