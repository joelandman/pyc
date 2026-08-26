# compiler/ — the rebuilt pyc

New tree. Contracts in `../rebuild/INTERFACES.md`, invariants in
`../rebuild/CHARTER.md`. The legacy tree under `../src/` is not carried
forward; see `../rebuild/ARCHITECTURE_REVIEW.md` for why.

## A1 — frontend (in progress)

```
pyc_parse/            run BY THE TARGET INTERPRETER (INTERFACES §2.1)
  encode.py           ast -> JSON, total by construction
  decode.py           JSON -> ast; reference semantics for the C++ reader
  __main__.py         python3.X -m pyc_parse FILE [--feature-version 3.Y]
tools/
  extract_schema.py   target interpreter -> normalized schema JSON
  gen_ast.py          schema -> include/pyc/ast/generated.hpp
  roundtrip.py        the totality proof (see below)
  lint_visitors.py    enforces the generic-arm ban
include/pyc/ast/
  support.hpp         hand-written: SourceLoc, Box<T>, ConstantValue, ov
  generated.hpp       GENERATED — do not edit, regenerate
```

### Regenerating

```bash
SYSROOT=~/opt/py-sysroots/cp314-3.14.7-tier1
$SYSROOT/bin/python3.14 compiler/tools/extract_schema.py -o /tmp/schema.json
./compiler/tools/gen_ast.py /tmp/schema.json -o compiler/include/pyc/ast/generated.hpp
```

A new CPython node kind then breaks every non-exhaustive `std::visit`. That is
I4 working; fix the arms, never add a generic fallback.

### Why the encoder is reflective

`encode.py` walks `_fields`/`_attributes` and special-cases nothing per node
type. The old converter hand-listed fields per node and dropped the rest —
which is why `x: int = 5` evaluated to `None`, `def g(a, /, b)` crashed
codegen, and `posonlyargs`/`kwonlyargs`/annotations were never read at all.

Two places JSON quietly loses Python, both handled:

- **`int` is arbitrary precision.** Encoded as a decimal *string*; a JSON
  number silently truncates above 2^53. `ConstBigInt` keeps it as text in C++
  too — storing it in an `int64_t` is exactly how the old runtime wrapped
  `math.factorial(25)`.
- **`float` via `repr`**, so `inf`/`nan` and exact round-trips survive.
  `bytes` are base64; `complex`/`tuple`/`frozenset`/`Ellipsis`/`None` are each
  tagged, and `bool` is tested before `int` because it is a subclass.

### The totality proof

```bash
./compiler/tools/roundtrip.py --stdlib
```

parse → encode → real JSON → decode → compare `ast.dump(include_attributes=True)`.
Comparing *with* attributes is stricter than required: it proves source
locations survive, which `SourceLoc` and `-g` depend on. Files that do not
parse under the interpreter are skipped and excluded from the denominator —
`Lib/test` ships deliberate bad-syntax fixtures.

### Deliberate over-inclusion

The schema keeps `AugLoad`, `AugStore` and `Param`, which are vestigial in
modern Python but remain real classes. Over-inclusion costs a dead visitor arm;
under-inclusion would be a silent miss. I1 makes that trade obvious.

## Totality result (2026-08-22)

`INTERFACES.md` §2.4 requires the round-trip to hold on **two** targets.

| Target | Files | ok | mismatch | error | skipped |
|---|---|---|---|---|---|
| CPython 3.14.7 | 14,641 | **14,637** | 0 | 0 | 4 |
| CPython 3.13.15 | 2,582 | **2,578** | 0 | 0 | 4 |

Both **TOTAL**: every node, field, and source attribute survived
parse → encode → real JSON → decode → `ast.dump(include_attributes=True)`.

The corpora differ in size because the 3.14 install carries a full
scientific stack (numpy, sympy, torch, nuitka) while the fresh 3.13 sysroot has
only numpy. Both cover the whole stdlib and `Lib/test`. The 4 skips on each are
files that do not parse under that interpreter — deliberate bad-syntax
fixtures, reported rather than hidden.

Schema delta between the targets is exactly the expected two nodes
(`TemplateStr`, `Interpolation` — PEP 750), with no field drift. The 3.13
header contains no `TemplateStr`; both compile independently from one
toolchain. That is I8: targets are data.

### What the two targets caught

Only running the second one exposed that `Constant` was being **silently
dropped** from the 3.13 schema: CPython keeps `Num`/`Str`/`Bytes`/
`NameConstant`/`Ellipsis` as subclasses of `Constant` through 3.13 and removes
them in 3.14, so "has subclasses" read as "is abstract" and a 3.13 header would
have had no literals in the language at all. 3.14 alone was clean. One target
proves the schema; two prove I8.

`_ALIASES` in `extract_schema.py` is a **curated list with no programmatic
marker** — review it whenever the target version moves, because this failure
mode is silent by construction.

## C++ deserializer fidelity (2026-08-22)

| Target | Files | ok | mismatch | error | skipped |
|---|---|---|---|---|---|
| CPython 3.14.7 | 14,641 | **14,637** | 0 | 0 | 4 |
| CPython 3.13.15 | 2,582 | **2,578** | 0 | 0 | 4 |

Checked structurally: the C++ reader emits a node-kind histogram and CPython
counts the same tree, so any dropped subtree, field, or node kind changes a
count. **FAITHFUL on both targets** — 17,215 files, one toolchain, target
supplied as data.

### The three under-described corners of CPython's reflection

`_field_types` is the schema source, and it under-describes itself in three
places. Each was a silent guess that real code turned into a real bug, and each
is now a curated list whose *absence* fails loudly:

| Constant | Problem | Failure without it |
|---|---|---|
| `_ALIASES` | deprecated names are still subclasses (`Num` under `Constant` ≤3.13) | `Constant` dropped entirely from the 3.13 schema |
| `_NULLABLE_ELEMENTS` | `None` *inside* a list, not expressed by `list[ast.expr]` | `{**a, **b}` and kwonly-without-default shift every later element |
| `_OBJECT_FIELDS` | `object` means three different things | t-strings and `case None:` fail to deserialize |

All three must be reviewed when the target version moves. `_NULLABLE_ELEMENTS`
is empirical (a scan of all 14,641 files found exactly two fields);
`_OBJECT_FIELDS` is enforced — an unlisted `object` field makes `gen_ast.py`
refuse to generate rather than guess.

## A2 — C-API binding (in progress)

```
include/pyc/rt/capi.hpp         CApiSymbol (INTERFACES §4)
include/pyc/rt/capi_table.hpp   GENERATED — 813 symbols
src/capi.cpp                    lookup()
tools/gen_capi_table.py         refcounts.dat -> the table
```

Derived from CPython's own `Doc/data/refcounts.dat`, not hand-written.
Maintaining refcount contracts for 800+ functions by hand is the bookkeeping
that produces leaks, and CPython already ships the answer.

| | count |
|---|---|
| symbols | 813 |
| returns a new reference (`Owned`) | 312 |
| returns a borrowed reference | 52 |
| always returns NULL | 16 |
| steal-annotated | 6 |
| banned | 1 |

### What that file is and is not authoritative for

**Returns: authoritative.** `+1` Owned, `0` Borrowed, `null` always-NULL,
blank means not a `PyObject*`.

**Parameter stealing: absent.** Its own header says so, and it is verifiable —
`PyList_SetItem`'s stolen `item` records `0`, indistinguishable from a borrow.
Stealing is therefore a curated list in the generator, validated against the
data: a name that does not match makes the generator **refuse to run**. That
guard is not decorative — three of the first six entries silently failed to
match, because the doc's parameter names are not uniform (`PyList_SetItem`
calls it `item`; `PyTuple_SetItem` calls the same argument `o`). An unapplied
steal annotation makes lowering emit a `DECREF` on a stolen reference: a
double free.

`may_raise` is deliberately conservative — everything except `void` is assumed
fallible. A redundant error edge is something A3 can optimise away; a missing
one loses an exception, which is a silent wrong answer (I1).

`_BANNED` carries a reason and a replacement. `PyModule_AddObject` steals
*only on success*, so its cleanup path differs by outcome and cannot be one
static contract; `PyModule_AddObjectRef` never steals. A contract that cannot
be expressed statically is one we refuse rather than approximate.

### Two sources, merged

`refcounts.dat` is not exhaustive, so `Misc/stable_abi.toml` is merged in as a
second, independent source. It records which symbols exist, that they are
stable-ABI, and **when each was added** — but carries no refcount data.

| | count |
|---|---|
| union | 1,039 |
| in both | 556 |
| `refcounts.dat` only | 257 |
| stable-ABI only — ownership **Unknown** | 225 |

A stable-ABI-only symbol arrives `Ownership::Unknown` and is **not emittable**.
That is the point: the merge makes the gap *enumerable and blocked* rather than
invisible. It does not close it. `emittable()` is the single question lowering
asks, and it is false for both banned symbols and unknown ones.

`added` gives the C-API half of I8: `PyModule_AddObjectRef` is `added = 3.10`,
so `available_in(3, 9)` is false and targeting 3.9 with it is a version error
rather than a link failure.

Ownership for unknown symbols is curated **one at a time**, from the C-API
docs, and validated: an override for a symbol `refcounts.dat` already records
is treated as a *conflict* and refuses to generate, because one of the two must
be wrong and silently preferring either bakes in a contradiction. With 225
candidates, bulk-guessing would be the worst available trade — a wrong entry
leaks or double-frees.

So far exactly one is curated: `PyModule_AddObjectRef`, because `_BANNED`
points at it as the safe replacement for `PyModule_AddObject` and a
recommendation that cannot be emitted is useless.

### Entry point and refcount discipline

```
include/pyc/rt/entry.hpp     pyc_rt_main, pyc_rt_total_refcount
src/rt/entry.cpp             Py_Initialize -> body -> finalize, exit codes
src/rt/refcount_probe.cpp    the leak harness
```

A4 emits a `main()` that calls `pyc_rt_main` with the body A3 lowered. The
sequencing lives here so the initialise/finalise/error protocol is written and
verified once. PEP 587 `PyConfig` is used because the pre-587
`Py_SetProgramName` family is removed in 3.13+, so a compiled binary has no
choice. `parse_argv = 0`: the binary is a program, not an interpreter, and must
never be steered into running something else by its own argv.

Exit-code semantics match CPython exactly, verified case by case:

| body raises | pyc | CPython |
|---|---|---|
| `SystemExit(3)` | 3 | 3 |
| `SystemExit()` | 0 | 0 |
| `SystemExit('bye')` | 1, message to stderr | 1, message to stderr |
| `ValueError('boom')` | 1, traceback | 1, traceback |

Testing that required care worth recording: `PyRun_SimpleString` handles
`SystemExit` *itself*, by calling `exit()` directly. A test built on it shows
the right exit codes while never running `pyc_rt_main`'s handler at all. The
test therefore uses `PyRun_String` and returns `-1` with the exception still
set, which is exactly the shape A3-lowered code will have.

### The leak harness

```
$ refcount_probe
  clean        delta=    +0 over 2000 iters  (+0.000/iter)  OK
  steal        delta=    +0 over 2000 iters  (+0.000/iter)  OK
  borrow       delta=    +0 over 2000 iters  (+0.000/iter)  OK
  overrelease  delta=    +0 over 2000 iters  (+0.000/iter)  OK
  leak         delta= +2000 over 2000 iters  (+1.000/iter)  OK  [expected to leak]
```

The **slope** is the signal, not the absolute total: a leak grows linearly with
iterations. Warm-up iterations are discarded because interned strings, cached
small ints and lazy imports legitimately raise the total once and never again;
counting that as a leak would make every correct program look broken.

`leak` is a deliberately incorrect workload that must FAIL. A leak detector
that has never detected a leak is not evidence of anything, so the harness
proves it can fail before its zeros mean anything.

`pyc_rt_total_refcount()` returns **-1**, not 0, on a release build, and the
probe exits 2 rather than reporting success. A release build silently reporting
"no leaks" would be worse than having no check.

## First differential measurement of the new tree (2026-08-23)

`verify/corpus/language`, 713 programs, oracle CPython 3.14.7:

| verdict | count |
|---|---|
| `MATCH` | **270** |
| `COMPILE_ERROR` (P2) | 442 |
| quarantined (nondeterministic) | 1 |
| **P0 silent wrong answers** | **0** |
| **P1 crashes / hangs** | **0** |

Pass rate 37.9% of scored programs. The shape matters more than the number:
**everything the compiler accepts, it gets right; everything it cannot do, it
refuses loudly with a named construct and a line.**

Contrast the old tree on its own rescued corpus: 97.76% passing, but with five
P0 silent wrong answers and hardcoded expectations that had recorded the
compiler's bugs as correct. A high number over a curated subset was worth less
than a low number over real programs with no P0s.

Two of the matches are the exact silent wrong answers the review opened with:

| | new tree | CPython | old tree |
|---|---|---|---|
| `2 ** 200` | `1606938…301376` | `1606938…301376` | `0` |
| `len("héllo wörld")` | `11` | `11` | `13` |

Both correct for free, because the object model is CPython's.

## Where the number went (2026-08-23)

Same corpus, same oracle, after A3 was carried from "refuses most of the
language" to "refuses three constructs". The 2026-08-23 run below adds a second
corpus (`verify/corpus/programs`, whole programs rather than single features)
and 15 `match` cases.

| | language | programs | total |
|---|---|---|---|
| scored | 727 | 92 | 819 |
| `MATCH` | **721** | **84** | **805** |
| `STDERR_DIFF` (P3) | 6 | 8 | 14 |
| `COMPILE_ERROR` | 0 | 0 | 0 |
| **P0 silent wrong answers** | **0** | **0** | **0** |
| **P1 crashes / hangs** | **0** | **0** | **0** |
| pass rate | 99.2% | 91.3% | **98.3%** |

37.9% → 98.3%, and the shape held the whole way: no verdict worse than P3
survives. All 14 remaining differences are the same one thing — CPython's
caret/tilde annotation line (`~~~~~^^^`) inside a traceback. pyc emits the
frame, the file, the line number and the source text; it does not emit the
column ranges, which are reconstructed from bytecode positions that a compiled
binary does not have.

Constructs still **refused** (loudly, with a line and a construct name — a
diagnostic, not a metric):

- positional-only / keyword-only parameters (`def f(a, /, b, *, c)`)
- `raise ... from`
- `type` alias statements (PEP 695)

Everything else in the two corpora lowers: `./compiler/tools/coverage.py`
reports **728/728 (100.0%)** on the language corpus.

### Suspendable bodies are compiled by CPython

Generator expressions, `yield` functions, `async def`, `await`, `async for` and
`async with` are all handled by the same mechanism: the body is compiled to a
code object *at build time* by the target interpreter, marshalled into the
binary, and rebuilt at runtime with `PyFunction_New`. The object really is a
`generator` / `coroutine` / `async_generator`, so `type()`, `isinstance`,
`inspect.*`, `send`/`throw`/`close` and laziness are exact by construction
rather than re-derived. See `../rebuild/GENERATORS.md`, which also records the
chunked-materialization plan that was **retired** for violating I1 and I2.

### `match` (PEP 634)

All nine pattern kinds lower natively: value, singleton, capture, wildcard,
sequence (with star), mapping (with `**rest`), class (positional and keyword),
or, and as. Sequence and mapping tests read `Py_TPFLAGS_SEQUENCE` /
`Py_TPFLAGS_MAPPING`; class patterns read `_Py_TPFLAGS_MATCH_SELF` and
`__match_args__` rather than keeping a list of type names that would drift.

The binding model was **measured from CPython before it was implemented**, and
it is not the obvious one: captures go to SSA temporaries and are stored to
their names only after the *whole* pattern matches, and the guard runs after
those stores. So a failed pattern leaves nothing bound, but a failed guard
leaves its captures bound. A stress corpus of 12 adversarial cases found
exactly one divergence, in an error message: CPython pluralises on the
*accepted* count, not the given one — `One() accepts 1 positional sub-pattern
(2 given)`.

### Refcount defects (found 2026-08-23; BOTH FIXED 2026-08-25)

Defect 2 fixed in 778024b, defect 1 in the commit that added this line. The
write-up below is kept because the reasoning — particularly why the obvious fix
for each is unsound — is the part worth not relearning.

**Defect 1 (double free on propagating paths): fixed.** Promotion to the frame
is now a MOVE at all nine sites (`forget` then push), so a value sits in exactly
one ownership list per reference it holds, and `make_landing_pad` gives each
entry its own decref. The `release_once` de-dup that had masked the double free
is removed: with moves in place it would UNDER-release a value legitimately
holding two references. The comment that justified it claimed "the live set is a
set, not a bag", which was simply false — `mark_owned` is an unconditional
`push_back` and `forget` erases the first match and breaks.

Also corrected on the way: `lower_match` pushed its subject onto `frame_owned_`
unconditionally but released it under `if (owns(subject))`, so an unowned
subject would be decref-ed by a landing pad and not on the normal path. Push and
release now agree, and the three early-return `pop_back()` sites are guarded to
match.

Verified: `compiler/tests` 77 clean, corpus 8 malformed (all issue #7's
pre-existing "never released"), differential 99.04% (721/727) with ZERO crashes —
the decisive check, since with the de-dup gone any missed promotion site would
be a real double free rather than a masked one. A targeted probe raising out of
`for`, `with` and `match` matches CPython exactly.

#### Original write-up (kept for the reasoning)

**Status correction.** Defect 1 was partially mitigated in `600fa8e` by a
`release_once` lambda in `make_landing_pad` that de-duplicates SSA ids, and
neither this section nor the notes were updated. Read the two entries below
with that in mind: the double free is masked, the root cause is not fixed.

Verified 2026-08-25 against emitted IR for `match x: case [a, b]: raise ...`:

* **Defect 1 is masked, not fixed.** Promotion is still a COPY -- `mark_owned(v)`
  followed by `frame_owned_.push_back(v)`, one reference in two lists -- and
  `release_once` hides the resulting double decref. The de-dup is only correct
  while no value holds two GENUINE references. `owned_` is a bag, not a set
  (`mark_owned` is an unconditional `push_back`; `forget` erases the first
  match and breaks), so nothing structurally prevents that. The capture path
  at `lower.cpp:2049` emits `IncRef` + `mark_owned` on the same value and is
  the candidate; in the IR inspected it balanced immediately
  (`incref %8 / store.global / decref %8`), so an under-release was NOT
  reproduced. The prescribed fix stands: make promotion a MOVE (`forget` then
  push) at the ~10 promotion sites, then release each entry unconditionally
  and drop the de-dup.

* **Defect 2 is live and reproducible.** `%5 = call.capi "pyc_rt_match_sequence"`
  is marked `owned` and is decref'd ONLY in the failure pads (`unwind.2`,
  `unwind.3`). On the normal path -- `match.after`, and `match.body` through
  `raise` into `unwind.4` -- it is never released. It leaks on every successful
  match.

* **`check_ir_wellformed.py` does not catch defect 2.** It reports `leak.py ok`
  even though its own self-check proves it can detect a leaked owned value. Its
  stated invariant is "every owned result is released on the normal path exactly
  once", and `%5` violates that, so the checker has a specific blind spot worth
  finding -- it is the tool both defects were originally found with.

Reproducer kept at the end of this section.

#### Original write-up

Both were found by `tools/check_ir_wellformed.py`, and neither is visible in
the differential run — which is exactly why the static checker exists
alongside it.

**1. A propagating landing pad can decref the same value twice.** `owned_`
(statement temporaries) and `frame_owned_` (values that outlive the statement)
are not disjoint: a `for` iterator, a `with` exit callable, a `match` subject
and a class body's namespace are all marked owned by the expression that
produced them *and* pushed onto `frame_owned_`. `make_landing_pad` releases
both lists, so those values get two decrefs on any path that propagates an
exception out of the function. It is a double free, latent only because the
corpora do not raise out of those constructs.

The tempting fix — dedupe by SSA id inside the pad — is **wrong**: a value can
legitimately hold two references (an `IncRef` plus a second `mark_owned`, which
is how `case x:` captures the subject), and deduping would under-release those.
The correct fix is to make the two sets disjoint by construction: promotion to
`frame_owned_` must *move* the reference (`forget` then push), and the matching
scope-end release must become unconditional instead of `owns()`-guarded. That
touches every promotion site and its release, so it is its own change.

**2. The `match` helpers' result is never released.**
`pyc_rt_match_sequence` / `_mapping` / `_class` each return a new reference —
a tuple of extracted values, or a new reference to `None` for "did not match".
Sub-patterns borrow items out of that tuple, and the captures borrowed from it
must stay alive until the whole pattern has matched, so it cannot simply be
released at the end of the arm. Fixing it properly needs a cleanup chain: every
owned temporary registers a block that releases it and branches to the previous
failure target, so each failure path unwinds exactly what it created. Or-patterns
make this mandatory rather than optional — alternative *i*'s temporaries exist
only on alternative *i*'s path, so a single shared release list would decref a
value that was never created on the taken path.
