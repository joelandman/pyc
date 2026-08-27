# Agent Directives

How to instruct agents to build pyc without repeating the drift.

## The rule that matters most

**Do not give agents feature tickets. Give them layer contracts and
invariants.**

The previous project failed because work arrived as "make this snippet
compile". Each such ticket is locally rational and globally corrosive: the
cheapest way to satisfy it is a special case at the callsite, and 662 of those
produce a 13,437-line lowering pass with 815 string comparisons that cannot
be extended toward general Python.

Every directive below therefore has the same shape:

```
CONTRACT   what this layer must expose to the next (a type, an interface)
INVARIANTS which CHARTER.md rules bind this layer specifically
BANNED     the concrete anti-pattern from the old tree, named
DONE WHEN  a machine-checkable condition, never "tests pass"
NOT YOURS  decisions this agent must escalate rather than make
```

`NOT YOURS` is load-bearing. Most of the old tree's damage came from a local
decision (use int64 for `int`) that silently became an architectural one.

---

## The second rule: never guess — hypothesise and test

**A claim about this compiler is either measured or it is not made.**

Guessing is permitted only as a *search heuristic*, never as a conclusion, and
a failed guess is not evidence of anything. When an observation needs
explaining, the procedure is:

| step | what it produces |
|---|---|
| **1. Observe** | the raw artefact — the emitted IR, the exact diagnostic, the differing bytes. Not a summary of it, and not the bucket it was sorted into. |
| **2. Synthesise** | one or more hypotheses that *differ in what they predict*. If they all predict the same observation, they are one hypothesis. |
| **3. Predict** | for each, the specific thing that would be true if it held — and, as importantly, what would be true if it did not. |
| **4. Test** | the smallest experiment that discriminates between them. A control that must keep passing counts as part of the test. |
| **5. Claim** | only what the test showed. Anything else is reported as unknown. |

Step 2 is the one that gets skipped. An observation usually arrives already
wearing an explanation, and adopting that explanation without generating a
rival to it is the whole failure mode. If only one hypothesis comes to mind,
that is a signal to look harder at the artefact, not a signal that it is right.

**When guessing has failed twice, stop and read the artefact.** On 2026-08-26 a
`with`-lowering change produced `use of undefined value '%bb7'` across eight
`Lib/test` files. Six hand-written candidate reproducers all compiled cleanly.
Dumping the IR took one command and named the cause immediately: the offending
function was `def spam(self): return self._spam`, containing no `with` at all,
ending in a branch to a block belonging to a different function. Six guesses
cost more than the one measurement that worked.

**A control is not optional.** "This fix works" and "this fix works *and* the
thing it must not disturb still works" are different claims. The comprehension
cell fix was only interpretable because a generator expression reading the same
free variable kept passing; the nested-class fix, because a class *body*
reading the same local kept passing. Each control converted "closures are
broken" into a defect with a location.

**Check whether the defect predates the change.** Asking that question of the
`fin_stack_` bug turned "my change broke this" into "my change widened a
pre-existing break", which is a different fix and a different risk assessment.

This applies to the instrument as much as the compiler. Every instrument error
recorded in `verify/README.md` — 48 phantom regressions, a phantom P0, 23
phantom leaks, a 3.6-point "regression" that was parallelism — was a claim made
from an observation without a test that could have falsified it.

---

## Preamble to paste into every agent

> You are working on pyc, an AOT Python-to-native compiler. Read
> `rebuild/CHARTER.md` in full before your first edit; it is binding.
>
> Non-negotiable, in priority order: correctness, then completeness, then
> performance. If your change makes a program faster by making it behave
> differently from CPython, the change is wrong.
>
> If you cannot handle a construct, emit a compile-time diagnostic naming the
> construct and the source line. **Never** emit code that produces a wrong
> value, and never silently ignore part of the input. A silent wrong answer is
> the worst outcome available to you — worse than a crash, far worse than a
> refusal to compile.
>
> If satisfying your task requires adding a case to a dispatch chain on a
> method name, type tag, or node-type string, stop. That is the failure mode
> this project is rebuilding to escape. Escalate to the Architect instead.
>
> Never hardcode expected output in a test. Tests compare against the pinned
> CPython binary at runtime.
>
> **Never guess. Hypothesise and test.** Every claim you make about this
> compiler must be something you measured. When an observation needs
> explaining: write down the raw artefact, synthesise at least two hypotheses
> that predict *different* observations, run the smallest experiment that tells
> them apart, and report only what it showed. Say "unknown" rather than assert.
> If two guessed reproducers have failed, stop guessing and read the artefact —
> the IR, the diagnostic, the bytes. Include a control that must keep passing,
> and check whether the defect predates your change.

---

## A0 — Architect / Coordinator

**CONTRACT.** Owns `CHARTER.md`, the layer interfaces, and the pinned CPython
version. Sole authority to amend any of them. Sequences the other agents and
resolves cross-layer conflicts.

**INVARIANTS.** All. Enforces them at review time.

**BANNED.** Accepting work that adds a dispatch-chain arm. Accepting a test
with a hardcoded expected value. Approving any performance change that alters
an observable result.

**DONE WHEN.** Layer interfaces are written down and frozen before the layers
below them are implemented. **Done: `INTERFACES.md` v1, 2026-08-22.** A0's
remaining standing duty is to guard them — an interface change requires a
version bump and a changelog entry, and §5 changes silently invalidate every
recorded baseline.

**NOT YOURS.** Nothing — but amendments to CHARTER.md require the user's
explicit sign-off, since the charter encodes their product decisions.

---

## A1 — Frontend (typed AST)

**INTERFACE.** `INTERFACES.md` §2 (and §1.1 for diagnostics). Frozen; do not change it unilaterally.

**CONTRACT.** Expose a statically-typed AST, **generated per target** — from
the target interpreter's `_field_types` (3.13+) or its `Python.asdl` (<=3.12) —
plus a converter from that target's `ast.parse` output into it. One type per
constructor. Every field present. Sum types closed and exhaustively matched.

Parsing invokes the **target** interpreter and deserializes the result; `pyc`
itself links no libpython. See `VERSION_TARGETING.md`.

**INVARIANTS.** I1, I4, I8.

**BANNED.**
- A node carrying `std::string type` and a generic `children` vector. That was
  `ASTNode` and it is why `posonlyargs`, `kwonlyargs`, and annotations were
  silently dropped, why `x: int = 5` evaluated to `None`, and why `def g(a,/,b)`
  crashed codegen.
- Hand-maintaining the node definitions. They are generated. If CPython adds a
  node kind, the generator emits it and every non-exhaustive match **fails the
  build** — that is the mechanism that keeps the frontend total.
- A generic fallback that descends into `.body`/`.value` for unknown nodes.
  Unknown is impossible by construction; if it happens, abort loudly.
- Hardcoding a CPython version, or assuming the host's AST equals the target's.
  The version-variant surface is small (3.12->3.14 is two node kinds) but it is
  not empty, and approximating it is how silent wrongness returns.

**DONE WHEN.** Round-trip property holds: for every `.py` file in CPython's
`Lib/`, converting `ast.parse` output into the typed AST and unparsing it back
yields a tree that `ast.dump` compares equal to the original. This is a
machine-checkable totality proof over ~200k lines of real Python, and it is
the single highest-value early deliverable in the project.

It must hold for **two targets** (3.13 and 3.14) from one `pyc` binary, which
is what proves I8 rather than asserting it.

**NOT YOURS.** Which versions are supported. Anything below the AST.

---

## A2 — Object model & runtime binding

**INTERFACE.** `INTERFACES.md` §4. Frozen; do not change it unilaterally.

**CONTRACT.** Expose the CPython C-API to the rest of the compiler: object
representation, refcounting/lifetime discipline, exception propagation,
GIL handling, and the embedding entry point (`Py_Initialize`, frozen stdlib,
module import).

**INVARIANTS.** I1, I2, I3.

**BANNED.**
- Defining a `PyObject` layout. There is exactly one and CPython owns it.
- A closed integer type tag. Types are `PyTypeObject*`. User classes are
  first-class because they are ordinary CPython types.
- Reimplementing anything CPython already provides — `int`, `str`, `dict`,
  `list` semantics all come from libpython. The old tree reimplemented all of
  them and got each subtly wrong (int64 wrap, byte-length `str`, ASCII-only
  `.upper()`).

**DONE WHEN.** A compiled binary can `import` a pure-Python stdlib module and
a precompiled C-extension wheel, and refcount discipline is verified under a
CPython debug build with `Py_REF_DEBUG` showing zero leaks on the test corpus.

**NOT YOURS.** Whether to link libpython (settled: yes). The unboxing strategy.

---

## A3 — Lowering & type inference

**INTERFACE.** `INTERFACES.md` §2, §3, §4. Frozen; do not change it unilaterally.

**CONTRACT.** Typed AST → typed SSA IR. Own the **type lattice** and the
dataflow analysis that proves types. Emit unboxed native operations where a
type is proved; emit C-API calls everywhere else.

**INVARIANTS.** I1, I2, I3.

**BANNED.**
- Dispatch chains on method-name or type-name strings. If the old tree needed
  a static guard (`check_dispatch_chain.py`) to find *unreachable arms* in its
  own dispatcher, the dispatcher was the bug.
- Special-casing a builtin. `sum()`, `len()`, `next()`, and a `for` loop must
  reach the same protocol lookup through the same path. The old tree's split
  between them is exactly why `__iter__` worked in comprehensions and failed
  in `sum()`.
- Unboxing without a proof or a guard. An unguarded assumption that a local is
  an int64 is how `math.factorial(25)` silently wrapped.

**DONE WHEN.** Unboxing is expressed as a lattice + transfer functions with a
written soundness argument, and every unboxed path has a test proving it falls
back correctly when the guard fails (e.g. an int that exceeds 64 bits mid-loop
must transparently become a bignum, not wrap).

**NOT YOURS.** The object model. The IR's consumers in codegen.

---

## A4 — Backend, linking & deployment

**INTERFACE.** `INTERFACES.md` §3, §5. Frozen; do not change it unilaterally.

**CONTRACT.** Typed SSA IR → LLVM IR → object code → linked executable. Owns
opt levels, `--static`, wheel discovery and linking, and `-g`/DWARF.

**INVARIANTS.** I1, I7, I8.

**PORT FROM THE OLD TREE.** The runtime-to-bitcode + LTO design (it is what
lets C-API call overhead get inlined away — now more valuable than before,
since the C-API is the fallback path), the CLI shape, and the working
`--static` pipeline.

**BANNED.** Letting the deployment paths rot behind the language work. They
are the product.

**DONE WHEN.** CI produces, per commit: a dynamic binary; a **Tier-1** static
binary (static libpython, `-rdynamic`) that `ldd` reports as having no
`libpython` dependency; and a binary that loads a real precompiled wheel and
runs it.

**Tier 2 (fully static) is DEFERRED — do not implement it.** It needs its own
sysroot (see below) and it can never load a C-extension wheel (CHARTER I7). It
is unblocked only after the wheel milestone in A2 is proven, at which point the
user decides. Until then, `--static` means Tier 1. Do not add a `-static` link
mode, and do not treat its absence as a gap.

No CPython rebuild is required for Tier 1: `libpython3.14.a` already ships in
`LIBPL` of the existing 3.14.7 install, and was verified working on 2026-08-22.

**NOT YOURS.** Semantics. If a program is wrong, that is A2/A3; do not paper
over it in codegen.

---

## A5 — Verification (independent)

**INTERFACE.** `INTERFACES.md` §5 — and nothing else. Frozen; do not change it unilaterally.

**CONTRACT.** Owns the differential harness and the completeness metric. This
agent **reports to the user, not to the feature agents**, and its results may
not be overridden by them.

**INVARIANTS.** I5 (including **I5a**), I6, I9.

**Deliverables.**
1. **Differential harness.** Runs a program under pyc and under the pinned
   CPython; compares stdout, stderr, and exit status. No expected values are
   ever written down.

   Per **I5a**, the comparison first collapses values whose *shape* cannot be
   stable between two runs — durations, heap addresses, and, for a case whose
   oracle has demonstrated time-dependence, clock readings. Shapes only, never
   values; symmetric; recorded per case. "Any divergence fails" was the
   original wording and it was wrong: measured on `Lib/test/`, it failed 94 of
   389 files that were byte-identical apart from `Ran 70 tests in 0.098s`.

   **The instrument measures; it does not infer.** Flags follow mechanically
   from what was observed. No severity ranking may be reintroduced: ranking
   verdicts and comparing ranks is what reported 48 fixed compile errors as 48
   regressions. Nothing may be dropped from the denominator either — an
   unreproducible case is reported, not quarantined.
2. **Corpus, not snippets.** Seed from real Python: CPython's `Lib/test/`,
   then real packages. Generated/property-based inputs welcome. Inline snippet
   pairs are banned outright — that format is what let a subset stay green.
3. **The metric.** CPython `Lib/test/` pass rate, in CI, in the README,
   monotonic. Publish it low and honest from day one.
4. **A silent-wrong-answer detector.** Any case where pyc exits 0 and prints
   something different from CPython is the top defect class (CHARTER I1). Note
   that this is a *measurement*, not a ranking: `exit == 0` and stdout differs
   are both recorded numbers, so it survives the ban on severity scales above.
   The old tree had at least five such cases reachable in three lines of
   Python.

**DONE WHEN.** The metric exists, runs per commit, and blocks regressions.
This agent's harness should exist **before** A1–A4 write substantial code.

**NOT YOURS.** Fixing the compiler. You measure and report; you do not
negotiate the bar down.

---

## Suggested sequencing

1. **A5 harness** and **A0 interfaces** first — the bar and the contracts must
   predate the code, or drift restarts on day one.
2. **A1** to the `Lib/` round-trip proof. This alone establishes "we can
   represent all of Python", which the old tree never could.
3. **A2** to "imports a wheel". This is the riskiest assumption in the plan;
   prove it early, while it is still cheap to change course.
4. **A4** static/wheel CI, so deployment never rots.
5. **A3** last and continuously — unboxing is where performance is won, and it
   is safe to defer precisely because I2 means the correct boxed path always
   exists underneath.

Note that performance work is scheduled last. Under this charter that is not a
sacrifice: the C-API fallback is always correct, so speed is a monotonic
improvement on a working compiler rather than, as before, a constraint that
silently redefined the language.
