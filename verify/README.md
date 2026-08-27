# verify — differential measurement (agent A5)

Owns the correctness bar. Reports to the user, not to the feature agents;
its results may not be overridden by them (`rebuild/AGENT_DIRECTIVES.md`, A5).

## The one rule

**No test in this tree stores an expected output.** The oracle is a real
CPython binary, executed at run time, every run. There is deliberately nowhere
in `measure.py` to put an expected value.

The volatile-text rules below are not an exception, and CHARTER **I5a** is the
authority for them: they name *shapes* that cannot be equal twice, never
values. I5 originally demanded that **any** divergence fail. Measured, that
reading failed 94 of 389 `Lib/test` files which were byte-identical to CPython
apart from `Ran 70 tests in 0.098s`. A contract that requires an instrument to
report 24% of a corpus as broken when it is not is a defect in the contract.

This is CHARTER I5, and it exists because of how the old tree drifted: 662
inline `(source, expected)` pairs, with the expected string documented as the
source of truth. Such a suite measures "does the curated subset still work" and
stays green forever while the gap to real Python never narrows.

## Usage

```bash
./verify/measure_run.py --corpus tests/                  # a directory of programs
./verify/measure_run.py --libtest                        # CPython Lib/test — the metric
./verify/measure_run.py --file prog.py --show 4          # one program, with evidence
./verify/measure_run.py --corpus tests/ --fail-on-silent-wrong
./verify/measure_run.py --corpus tests/ --json out.json  # machine-readable

./verify/measure_compare.py --baseline b.json --current out.json
```

Two programs, and the split is the point. `measure_run.py` **measures** and
writes down what it saw. `measure_compare.py` **compares** two of those
records. Nothing in the measuring half forms an opinion about what a
difference means, and nothing in the comparing half re-runs anything.

The subject comes from `--pyc`, `$PYC_BINARY`, or `./compiler/tools/pycc`. The
oracle comes from `--oracle`, or `--sysroot DIR` (read from that sysroot's
`pyc-sysroot.json`), or the running interpreter. Prefer `--sysroot`: a
divergence must be measured against the runtime actually being targeted, and
that makes the parse oracle and the differential oracle the same binary
(`rebuild/VERSION_TARGETING.md`).

## Flags — measurements, not a severity scale

Each case runs the oracle, compiles, runs the binary, then runs the oracle
again. The flags follow from what those four steps produced, mechanically. A
case carries any number of them, or none.

| Flag | What was measured | Counts against the rate |
|---|---|---|
| `DID_NOT_COMPILE` | pyc exited nonzero, or produced no binary | yes |
| `TIMEOUT` | the binary exceeded the run timeout | yes |
| `STDOUT_DIFFERS` | the binary's stdout ≠ CPython's | yes |
| `EXIT_DIFFERS` | the binary's exit status ≠ CPython's | yes |
| `ORACLE_UNSTABLE` | CPython disagreed with **CPython** across the two runs | yes |
| `STDERR_DIFFERS` | right answer, different diagnostics | no |

There is no ranking here, and that is deliberate. The previous design ranked
verdicts by severity and detected regressions by comparing ranks; because
`COMPILE_ERROR` ranked "better" than `CRASH`, a batch of **fixed** compile
errors — files that finally began running — was reported as **48 regressions**.
The ranking was the defect. Nothing replaced it, because a rank is an opinion
and this layer holds none.

**stderr does not count against the rate** because it does not change what the
program computed. It is still recorded and still shown; a traceback whose
wording drifts is worth seeing and is not worth failing a build over.

`ORACLE_UNSTABLE` **does** count. A program whose own output changes between
two runs of the same interpreter has no ground truth, and that is a fact about
the program or the environment worth reporting — not a reason to quietly drop
it from the denominator.

### Silent wrong answers are measured, not inferred

CHARTER I1's top offence — the binary exits 0 and prints the wrong thing — is
two recorded numbers, not a judgement: `exit == 0` **and** `STDOUT_DIFFERS`.
Both the runner (`--fail-on-silent-wrong`, needs no baseline) and the
comparator (a *new* one fails the gate) read it straight off the record.

Running `verify/corpus/known-gaps` re-derives, from measurement alone, exactly
the three P0s from issue #9 and nothing else:

```
globals_none_p0.py  line 1: cpython 'False'      pyc 'True'
locals_bool_p0.py   line 1: cpython 'True'       pyc 'False'
locals_none_p0.py   line 1: cpython "{'a': 1}"   pyc 'None'
```

**A compile error is not a bug.** Refusing an unsupported construct is the
*correct* behaviour (I1: fail loudly, never silently). Coverage gaps should
present as `DID_NOT_COMPILE` and get resolved by implementing the feature —
never by downgrading the check.

## Gate outcomes (`measure_compare.py`)

Exactly two things fail the gate:

1. **A case that passed now fails** — it had no rate-counting flag, and now
   has one.
2. **A new silent wrong answer** — a case that now exits 0 while its stdout
   differs (CHARTER I1).

A case that newly became `ORACLE_UNSTABLE` **and nothing else** is printed and
does not fail. pyc is not involved in that measurement — it comes from two
CPython runs — so calling it a compiler regression would be false. It still
counts against the pass rate, because the case genuinely has no ground truth
any more.

Everything else is printed as `changed` and fails nothing. A case that used to
be refused by the compiler and now runs and crashes swapped one loud failure
for another: it did not pass before and does not pass now, and the pass rate
already says so. Failing the build for that is how the 48 phantom regressions
happened.

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
missing baseline prints a note and returns **0** without it — a green run in which
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

So a run that is better than its baseline prints **BASELINE IS BEHIND**, with a
`::warning` annotation and a step-summary block naming the refresh command. It
never fails: blocking a change for being better would be absurd.

Staleness is suppressed when the run also regressed. A real failure must not be
softened by good news sitting next to it.

Refresh deliberately, after checking what moved:

```bash
./verify/measure_compare.py --update --baseline <baseline> --current <run>
```

## How nondeterminism is handled

### Values that cannot be equal twice are collapsed first

A program run at two different times, under two different loads, at two
different heap layouts, **must** print a different elapsed time, a different
clock reading and a different address. Comparing those bytes measures the clock
and the allocator, not the compiler. Six patterns are collapsed before
comparison — including the oracle-against-oracle one — but they are **not all
applied to every case**:

| rule | shape | becomes | applied |
|---|---|---|---|
| `elapsed` | `in 0.001s` (unittest's trailer) | `in <ELAPSED>s` | always |
| `heap_address` | `0x7f3c605a4c20` **only after `" at "`** | `0xADDR` | always |
| `asctime` | `Wed Aug 26 13:14:57 2026` | `<ASCTIME>` | on demand |
| `iso_datetime` | `2026-08-26T13:14:57.123` | `<ISOTIME>` | on demand |
| `clock_time` | `13:14:57` | `<TIME>` | on demand |
| `iso_date` | `2026-08-26` | `<DATE>` | on demand |

The split is the important part. An elapsed duration and a heap address are
never something a program deterministically computes and a test
deterministically checks, so collapsing them can hide nothing. **A date can
be.** `datetime` arithmetic on fixed inputs prints a fixed date, and that is
exactly the kind of answer this harness exists to check.

So the clock rules apply only to a case whose oracle has **demonstrated** that
its output depends on when it ran — by disagreeing with itself across the two
runs, once durations and addresses are out of the way. Which rules apply is
itself a measurement, not a guess.

`verify/corpus/language/case_509.py` is why this exists. It computes
`date(2024, 3, 15) + timedelta(days=10)` and prints the result. Under a blanket
date rule, pyc answering `2024-03-24` instead of `2024-03-25` compared **equal**
— the instrument would have certified a wrong answer, the exact failure CHARTER
I1 is about. Its oracle reproduces exactly, so it now gets a strict
byte-for-byte comparison and its date arithmetic stays checked. Meanwhile
`test_strftime`, whose oracle really does disagree with itself, gets the clock
rules and passes.

The guard rails matter more than the list:

- **Both sides get identical treatment.** Nothing is applied to pyc that is not
  applied to CPython.
- **Each rule matches one shape.** A value of that shape collapses; anything
  else is untouched. `Ran 6 tests` still differs from `Ran 7 tests`, and
  `0xDEADBEEF` in a program's own output is not an address and is left alone.
- **Raw output is what gets stored and shown.** Normalization decides only
  whether a difference is *reported*.
- **Every rule that fires is named in the record** (`normalizers` per case,
  plus `oracle_varied`, and a summary in the run), so a masked difference can
  be traced to its mask.

**The cost, stated plainly:** in a case whose oracle *did* vary, a pyc bug
producing a well-formed wrong timestamp is invisible. That case had no stable
ground truth for that value to begin with, so nothing was traded away — but the
list stays short and anchored regardless, and nothing joins it without a
measured case that needs it.

What is deliberately **not** normalized, though it also varies: pids, ephemeral
ports, temp directory names, and anything from an unseeded `random`. Those are
the program choosing to print a coin flip, not an artefact of when the
measurement ran, and they stay visible as `ORACLE_UNSTABLE`.

This replaced a blanket "no normalization" rule. The reasoning for that rule —
hand-written normalizers mask real divergences — is sound and is why the list
is six anchored patterns rather than a free hand to rewrite output. But the
rule as written made the instrument compare the clock against itself and
report the result as a compiler property, which is worse.

### Timeouts get a second attempt at double the limit

A timeout is as much a statement about the machine as about the program: a
loaded runner, a cold cache, a slow first import. So any run that times out is
retried once at **2×** the limit. Only if that also expires is the case a
`TIMEOUT` — and it is reported as one on whichever side it happened, oracle
included. There is no such thing as an "oracle failure" here; CPython taking
too long is a fact about the timeout, not a broken measurement.

**A timeout where CPython itself did not finish never fails the gate.** Six
`Lib/test` files are in that state, and `test_zipfile64` is the honest example:
it builds multi-gigabyte archives and takes **107 s** under CPython on this
machine, measured, against a 30 s limit and a 60 s retry. pyc is not what
failed. The case still counts against the pass rate — nothing was scored — but
blaming the compiler for it would be false, exactly as with `ORACLE_UNSTABLE`.
Each record carries `oracle_timed_out` so the comparator can tell the two
apart.

### An unstable oracle is reported, never dropped

The harness **runs the oracle twice — before and after the subject,
spanning compilation** — and flags any case whose own output is not
reproducible *after normalization* as `ORACLE_UNSTABLE`. It is reported, not excluded: dropping a
case from the denominator is a decision the instrument should not be making on
its own, and the old quarantine silently discarded 82 of 389 `Lib/test`
measurements it had already taken.

The gap matters. Two back-to-back oracle runs agree on a program that prints
the wall-clock time; the run *after* compilation does not. `test_strftime` was
briefly reported as a P0 for exactly this reason.

`verify/corpus/language/case_507.py` is the standing example: it prints
`<itertools.chain object at 0x…>`, so its own address makes it unmatchable by
construction. That is a corpus defect, and it now shows up as one instead of
vanishing into a quarantine bucket.

### What is actually nondeterministic in `Lib/test`, measured

The 2026-08-26 nightly quarantined 86 files. That number invited the reading
"CPython is nondeterministic", which is not what it said. Re-running the
oracle four times per file, with `PYTHONHASHSEED=0` pinned, isolates six state
sources — every one of them inside the test, none of them CPython being
unreliable and none of them pyc:

| state source | files | stream |
|---|---|---|
| elapsed wall time — unittest's `Ran N tests in 0.004s` | all 18 | stderr |
| ASLR heap address in a repr | `test_audit`, `test_format` | stdout |
| unseeded `random` | `test_sort`, `test_imaplib`, `test_thread`, `test_threading` | stdout |
| thread scheduling order | `test_thread`, `test_threading` | stdout |
| process id | `test_pty` | stdout |
| ephemeral TCP port | `test_socketserver` | stdout |
| temp directory name | `test_zipimport_support` | stdout |
| wall-clock date | `test_strftime` | stdout, intermittent |

Nine of the eighteen vary **only** on stderr, and all nine of those are pure
elapsed-time text: `test_codecmaps_{cn,hk,jp,kr,tw}`, `test_ucn`,
`test_unicodedata`, `test___all__`, `test_strftime`. Under this instrument they
are `STDERR_DIFFERS` — recorded, shown, and not counted. The old harness
quarantined them, which cost their whole measurement.

**Whether a probe SEES the instability is chance, and that produced a false
signal.** `test_strftime` printed identical stdout across four back-to-back
runs here, yet the nightly caught it: its two oracle samples straddled
compilation and crossed a second boundary. The same asymmetry made the compiled
binary look *less* deterministic than CPython when both were doing the same
thing — the subject got three samples and the oracle two, so 68 files were
labelled "the compiled program does not reproduce across runs". Compiling
`test_copyreg` and `test_codeop` and running each five times shows the only
variation is `Ran 6 tests in 0.000s` → `0.001s` on stderr. Nothing in pyc's
output was nondeterministic in any of them.

Collapsing volatile text settles most of this table. Measured on the six worst
offenders, the change is not marginal:

| case | before | after |
|---|---|---|
| `test_strftime` | quarantined | **passes** |
| `test_codecmaps_kr` | quarantined | **passes** |
| `test_ucn` | quarantined | `EXIT_DIFFERS` — a real failure it was hiding |
| `test_unicodedata` | quarantined | `EXIT_DIFFERS` — a real failure it was hiding |
| `test_socketserver` | quarantined | `ORACLE_UNSTABLE`, evidence names the port |
| `test_audit` | quarantined | `DID_NOT_COMPILE`, with the line and construct |

Two clean passes recovered, and two genuine compiler failures that quarantine
had been swallowing. Only the cases whose answer really does contain a coin
flip — a pid, a port, a temp path, an unseeded `random` — remain
`ORACLE_UNSTABLE`, counted against the rate and honest about why.

`PYTHONHASHSEED=0`, `PYTHON_COLORS=0` and `NO_COLOR=1` are pinned for **both
sides**. This is separate from the collapsing above: it fixes what the programs
are run *under*, so a difference is a difference in the programs rather than in
the invoking shell.

## Where the temporary files go

Every case compiles into its own temp directory, and a compiled binary is
~32 MB. A full run therefore churns gigabytes, and on a machine where `/tmp` is
a tmpfs it will fill it.

**That failure mode is dangerous, because it does not look like itself.** When
the disk is full the linker fails, the harness records `DID_NOT_COMPILE`, and
the rate collapses — which is indistinguishable from a catastrophic compiler
regression unless someone reads the diagnostic:

```
error: unable to open output file '…/support-eafbd5.o': 'No space left on device'
```

Both `pycc` (`mktemp -d`) and the harness (Python's `tempfile`) honour
`TMPDIR`, so point it at a real disk:

```bash
export TMPDIR=~/temp        # not /tmp, which is a tmpfs on this machine
```

Before believing any large drop in `DID_NOT_COMPILE` terms, check `df` and grep
the diagnostics for `No space left`. This has bitten twice.

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
   unittest timing text and is the reliable signal for "did this break
   something".

`measure_run.py` records `jobs` and the oracle's `-VV` banner in its output,
and `measure_compare.py` refuses outright to compare across a different oracle
or different compiler flags, so a number cannot silently be compared against one
taken under different conditions.

This is written down because it was learned the expensive way: a 13.11% ->
9.51% "regression" was reported here as real, and a control run at `--jobs 4`
reproduced 13.11% exactly from the same binary. The reported drop was entirely
measurement noise.

## The metric (CHARTER I6)

**The published number is `passing / len(corpus)`** — of the 389 `Lib/test`
files, how many showed no rate-counting difference from CPython. The
denominator is every file measured. Nothing is excluded, so the number can move
only when the compiler does.

There is no second, more flattering figure any more. The old one divided by the
*scored* set, which excluded quarantined cases, and quarantine membership was
nondeterministic — so it moved for reasons unrelated to the compiler, and moved
**backwards**: a run with less flakiness scored more cases and therefore
reported a *lower* rate. The first scheduled metric run failed on exactly that,
reporting a regression from 1.96% to 1.94% while matching the identical 7 cases.


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
- New compiler CLI → `Compiler` in `measure.py` is the only place that knows
  how pyc is invoked, so the harness survives the rebuild.
- New kind of difference → a flag in `measure.Flag`, set from a recorded fact.
  If setting it requires guessing *why* the difference happened, it does not
  belong in `measure.py`.
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

The harness flagged all five as P0 when this was written. They have since been
fixed: as of the schema-3 measurement, `case_273`, `case_274` and `case_525`
carry only `STDERR_DIFFERS` — pyc now raises what CPython raises, and only the
traceback wording differs. The section stays because the *reason* CHARTER I5
forbids stored expectations does not expire.
