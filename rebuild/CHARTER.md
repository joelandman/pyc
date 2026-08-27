# pyc — Project Charter

This document is **binding on every agent and human working on the project**.
It exists because the previous incarnation of pyc drifted from "a general
Python compiler" to "a compiler for a curated subset" without anyone deciding
to do so. The invariants below are the constraints whose absence permitted
that drift. They are not style preferences. A change that violates one is
rejected regardless of what it makes pass.

## 1. Product definition

pyc compiles Python programs to native executables that are easy to deploy.

Three properties, in priority order when they conflict:

1. **Correctness** — the compiled program behaves as CPython does.
2. **Completeness** — the set of programs pyc accepts approaches "real Python".
3. **Performance** — compiled programs are faster than CPython.

Performance is last on purpose. The previous project inverted this and bought
speed with semantic divergence (machine-word `int`, byte-oriented `str`); the
result was silent wrong answers and a permanent ceiling on completeness.

## 2. Architecture decision (settled — do not relitigate)

**pyc adopts CPython's object model and links libpython.**

The runtime value representation *is* `PyObject` as CPython defines it, ABI
compatible. pyc does not define its own object layout.

This buys, at a stroke and without further work: arbitrary-precision `int`,
correct Unicode `str`, `dict` ordering and hashing, the full descriptor/MRO
protocol, the real standard library, and **precompiled C-extension wheels
(PyTorch, NumPy)**. These are precisely the things the previous runtime could
never have reached — it was a 208-byte flat struct with a closed integer type
tag and no type object.

The compiler's job is therefore *not* to reimplement Python. It is to turn
Python-level code into native code, using unboxed native representations
wherever types can be **proved**, and falling back to the C-API everywhere
else. Speed comes from proving types, never from redefining them.

**Validated 2026-08-22.** This is no longer an assumption. A Tier-1 binary
(static libpython, `-rdynamic`, no libpython dynamic dependency) imports and
runs real C-extension wheels: **NumPy 2.5.2** and **PyTorch
2.11.0.dev+rocm7.0** (`matmul=[[5.0,14.0],[14.0,50.0]]`). PyTorch is close to
the harshest case available — a ROCm build with one of the largest `.so`
graphs in the ecosystem. `tools/build-python-sysroot.sh` now proves this on
every sysroot build (`--require-wheel` makes it fatal; the nightly metric uses
it), so the capability cannot silently regress.

Rejected alternatives, for the record: an independent semantically-correct
runtime (forecloses wheels forever), and a hybrid unboxed/CPython boundary
(the cpyext problem — highest ceiling, but the boundary is where correctness
and performance both go to die). The hybrid may be revisited *only* after the
completeness metric in §4 exceeds 80%.

## 2a. Implementation decisions (settled — do not relitigate)

- **C++20.** Chosen for first-class LLVM API access and native C-API interop,
  which matters because A2's entire job is talking to libpython.
- **I4's totality is enforced, not merely intended.** Verified on this
  toolchain: a `std::visit` over an overload set that omits an alternative is a
  hard **compile error**, not a warning. Generated `std::variant` AST nodes
  therefore give I4's "adding a node kind breaks the build" guarantee natively.
  The one hole is a generic `auto` arm, which compiles and silently swallows —
  so **generic arms are banned in visitors over `pyc::ast` / `pyc::ir`**, and a
  build lint enforces it. See `INTERFACES.md` §2.5.
- **JSON across the parse boundary**, revisitable on profiling evidence; it is
  an implementation detail behind `INTERFACES.md` §2.1.

Layer interfaces are frozen in **`INTERFACES.md` (v1)**. Amending them requires
A0 sign-off and a version bump.

## 3. Invariants

Each inverts an observed failure of the previous tree. Cited failures are
measured from the previous `build/pyc`, not hypothesised.

### I1 — No silent wrong answers. Ever.

A program must produce CPython's answer, a compile-time diagnostic, or a real
Python exception. It must **never** produce a wrong value.

> Previously: `x: int = 5` printed `None`; `math.factorial(25)` printed a
> wrapped int64; `len("héllo wörld")` printed 13.

Corollary: any AST node, type, or construct the compiler cannot handle is a
**hard compile error naming the construct, the line, and the reason**. Silent
fallthrough is the single most damaging pattern in the old tree and is banned.

### I2 — No semantic divergence for performance.

`int` is arbitrary precision. `str` is a sequence of code points. Float is
IEEE-754 double as CPython specifies. No optimization may change an observable
result. Unboxing must be guarded by a proof or a runtime check that restores
the boxed path.

### I3 — Protocols, never callsites.

A language feature is implemented **once**, in terms of the object protocol
(`__iter__`, `__next__`, `__getitem__`, `__eq__`, `__hash__`, …). It is never
implemented at an individual call site or builtin.

> Previously: `__iter__` worked inside a comprehension but not in `sum()`;
> generators worked in `list()` but `next()` returned `None`. `Compiler.cpp`
> held 815 string comparisons, 99 in one `methodName == "..."` chain, plus a
> static test guard (`check_dispatch_chain.py`) to detect *dead arms* in it.
> If you find yourself adding an arm to a dispatch chain, you are violating I3.

### I4 — The AST is typed and total.

The frontend consumes CPython's own `ast` output and converts it to a
**generated, statically-typed** AST — generated from CPython's `Python.asdl`
for the pinned version, never hand-maintained. Every field of every node is
represented. Adding a node kind upstream must break the build, loudly.

> Previously: a stringly-typed `{type, value, op, id, args, children}` node.
> `FunctionDef` flattened body, defaults, and decorators into one `children`
> vector to be re-sorted by string compare. `kwonlyargs`, `posonlyargs`, and
> annotations were never read; unknown nodes hit a generic fallback that
> descended into `.body`/`.value` and discarded the rest.

### I5 — Every test is differential against real CPython.

No test may hardcode expected output. Each runs under both pyc and the pinned
CPython binary, and the two are compared on stdout, stderr, and exit status.

> Previously: 662 inline `(source, expected)` pairs with expected hardcoded as
> "the source of truth". Such a suite stays green forever on a subset. This is
> the mechanism by which the general-purpose intent was lost: every ticket was
> "make this snippet pass", and 662 green cases feel exactly like progress.

#### I5a — A value that cannot be equal twice is not evidence.

**Amended 2026-08-26.** I5 originally read "**any** divergence in stdout,
stderr, or exit status fails". That was wrong, and it was wrong in the
direction that matters: it required the instrument to report failures that
were not failures.

A program's output can depend on state that necessarily differs between two
runs — elapsed time, a heap address, a pid, an ephemeral port, a temp path, an
unseeded `random`, thread interleaving. Two runs at different moments under
different load **must** produce different bytes for those. Comparing them
measures the clock, the allocator and the scheduler; it says nothing about the
compiler, and demanding equality guarantees a false report.

This is not theoretical. Measured on `Lib/test/` at 389 files, the strict
reading discarded or failed **94 files — 24% of the corpus — that were
byte-identical to CPython apart from unittest's `Ran 70 tests in 0.098s`
trailer.** Spot-checked by hand, four of them had identical stdout, identical
exit 0, and a stderr differing only in that duration. The contract, read
strictly, was the defect.

So I5 compares **after collapsing values whose shape cannot be stable**, under
four constraints that keep it from becoming the hardcoding I5 exists to forbid:

1. **Shapes, never values.** A rule names a form — a duration, an address
   after `" at "`, an ISO timestamp. It never names an expected result. `Ran 6
   tests` still differs from `Ran 7 tests`.
2. **Symmetric.** Nothing is applied to pyc that is not applied to CPython.
3. **Earned, not assumed.** A rule whose shape could ever be a *stable*
   correctness property — a date, a clock reading — applies only to a case
   whose oracle has demonstrated, by disagreeing with itself across two runs,
   that its output depends on when it ran. Durations and addresses are never
   stable answers, so those always apply.
4. **Auditable.** Raw output is what gets stored and shown; every rule that
   fires is recorded per case. A masked difference can always be traced to the
   rule that masked it.

Constraint 3 is load-bearing and was learned the hard way:
`verify/corpus/language/case_509.py` computes `date(2024, 3, 15) +
timedelta(days=10)`. A blanket date rule made pyc answering `2024-03-24`
compare **equal** — the instrument certifying a wrong answer, an I1 violation
introduced by fixing this one. Its oracle reproduces exactly, so it is compared
byte-for-byte and its arithmetic stays checked.

What is **not** collapsed, though it also varies: pids, ports, temp paths and
unseeded `random`. Those are the program choosing to print a coin flip rather
than an artefact of when the measurement ran. They are reported as
`ORACLE_UNSTABLE` — counted against the rate, never silently dropped.

**Timeouts are a limit, not a verdict.** A run that exceeds its budget is
retried once at double it. Only if that also expires is the case a timeout, on
whichever side it occurred. "The oracle failed" is not an outcome: CPython
taking too long is a statement about the limit.

**The standing obligation.** Any corpus whose output depends on changeable
state needs this handling, and new corpora must be checked for it rather than
assumed clean. When a case is flagged, the first question is *which state
changed* — the answer is a property of the test, and belongs in the record
before any of it is read as a compiler property.

### I6 — Completeness is measured, published, and monotonic.

The north-star metric is **pass rate over CPython's own `Lib/test/` suite**.
It is computed in CI, published in the README, and may never regress. It will
start humiliatingly low. That is the point: it is the only number that cannot
be gamed by adding more snippets.

### I7 — Deployment is a first-class feature, tested like one.

There are **two static tiers**, they are not interchangeable, and CI must build
and run both:

- **Tier 1 — static libpython, dynamic libc** (`-rdynamic`). One binary, no
  libpython dependency, `dlopen` still available. **C-extension wheels work.**
  Verified working against the existing 3.14.7 build on 2026-08-22.
- **Tier 2 — fully static** (`-static`). No `dlopen` at all.

**Tier 2 and C-extension wheels are mutually exclusive.** This is a property of
ELF static linking, not a limitation to engineer around: a fully static binary
has no dynamic symbol table, so a `.so` can never resolve `libpython` symbols
from it. PyTorch in a fully static binary is impossible. Do not promise it.

Tier 2 additionally requires a **purpose-built sysroot** in which the stdlib
extension modules are compiled into `libpython` as builtins. In a stock build
they are not: measured on 3.14.7, **37 modules are builtin and 77 are `dlopen`
-ed `.so` files**, including `math`, `zlib`, `_socket`, `_ssl`, `_json`,
`array`, and `_struct`. A fully static binary built against a stock sysroot
links and starts, then fails on `import math`.

**Tier 2 is deferred by decision (2026-08-22).** Tier 1 unblocks all near-term
work, including A2's wheel milestone, which needs Tier 1 rather than Tier 2.
Building the Tier-2 sysroot before the wheel path is proven risks building it
twice. Revisit only after A2 demonstrates a loaded wheel. Until then `--static`
means Tier 1, and Tier 2's absence is not a gap.

CPython 3.14.7 source is retained at
`/home/joe/build/cpython-sysroot/Python-3.14.7` for that build when it happens.

Deployment is the product; it does not get to be an afterthought that rots.

### I8 — The compiler is version-parameterized; targets are data.

No CPython version is hardcoded anywhere in the compiler. A target is a
resolved **Python Target Description** (version, ABI, sysroot, interpreter,
generated AST schema), selected by `--python=X.Y`. Supporting a new CPython
release means adding a sysroot and regenerating a schema — never editing the
compiler.

The typed AST of I4 is generated **per target**: by introspecting
`_field_types` from the target interpreter (3.13+), or from `Python.asdl` in
that version's source tarball (<=3.12). A construct absent from the selected
target is a compile-time diagnostic naming the construct, the version that
introduced it, and the target — never a silent fallthrough.

See `VERSION_TARGETING.md` for the full design, the flag surface, and the
measured facts it rests on.

### I1a — A refusal is a hypothesis, not a resting place.

**Added 2026-08-27.** A compile-time refusal is the correct RESPONSE to an
unsupported construct (I1). It is not evidence that anything around it is
correct, and it is not a place to stop looking.

Measured over five consecutive features implemented on 2026-08-26/27, **four
refusals were concealing a live defect**:

| refusal | what it was hiding |
|---|---|
| `return`/`break`/`continue` in `with` | the guard scanned only DIRECT children, so a `return` nested one level deep compiled and skipped `__exit__` — a P0, in the guard whose stated purpose was preventing exactly that |
| annotated assignment | unreachable code after a terminator emitted landing pads referencing dropped values; the module would not assemble |
| `metaclass=` | `type.__new__`'s implicit wrappers were skipped for every compiled class ever produced, because it tests `PyFunction_Check` and a pyc callable is not one. A four-line `__init_subclass__` program SEGFAULTED |
| `raise … from` | an exception leaving an `except` handler never popped its `exc_info`, so every later raise in the program inherited a stale `__context__` |
| positional-only parameters | nothing — the refusal was honest |

So treat every refusal as fruitful ground, by two mechanisms that both paid:

1. **Probe the guard's boundary.** A guard states what it rejects; test what it
   does NOT. The `with` guard's boundary was "direct children only", and one
   nested `return` walked straight past it.
2. **Look at what becomes reachable.** Removing a refusal lets code run that
   never ran before, and what it does there is new evidence. Annotated
   assignment surfaced the unreachable-code bug this way; `metaclass=` turned a
   segfault into a legible error that named its own cause.

A refusal that turns out to conceal nothing is a fact worth recording too —
say so, as positional-only does above.

### I9 — A claim about this compiler is measured, or it is not made.

**Added 2026-08-26.** Never guess. Guessing is permitted as a *search
heuristic* and never as a conclusion; a failed guess is evidence of nothing.

When an observation needs explaining:

1. **Observe** the raw artefact — the emitted IR, the exact diagnostic, the
   differing bytes. Not a summary of it, and not the bucket it was sorted into.
2. **Synthesise** hypotheses that *differ in what they predict*. Ones that
   predict the same observation are one hypothesis. This step is the one that
   gets skipped: an observation arrives already wearing an explanation, and
   adopting it without generating a rival is the failure mode itself. If only
   one hypothesis comes to mind, look harder at the artefact.
3. **Predict** what each implies would be true — and what would be true if it
   did not hold.
4. **Test** with the smallest experiment that discriminates. A control that
   must keep passing is part of the test, not a nicety.
5. **Claim** only what the test showed. Otherwise report it as unknown.

**After two failed guesses, read the artefact.** Six hand-written reproducers
for a `use of undefined value '%bb7'` failure all compiled cleanly; dumping the
IR took one command and named the cause — a function containing no `with`
ending in a branch to another function's block.

**A control converts "X is broken" into a defect with a location.** The
comprehension-cell P0 was interpretable only because a generator expression
reading the same free variable kept passing; the nested-class defect, because a
class *body* reading the same local kept passing.

**Ask whether the defect predates the change.** For `fin_stack_` that turned
"my change broke this" into "my change widened a pre-existing break" — a
different fix, and a different risk.

This binds the instrument as tightly as the compiler. Every instrument error on
record — 48 phantom regressions, a phantom P0, 23 phantom leaks, a 3.6-point
"regression" that was parallelism, a mis-attributed quarantine bucket — was a
claim made from an observation with no test that could have falsified it.

### Deferred: per-function Python frames (decided 2026-08-25)

Compiled functions push no Python frame, so `sys._getframe(N)` does not track
Python call depth. `sys._getframe` itself raises rather than lying, so this is
I1-clean at the boundary — but stdlib CALLERS degrade quietly around it, and one
is already a measured P0 (issue #9): `doctest._normalize_module` walks up one
frame too few, resolves the wrong module, and returns an empty test suite, so a
compiled `Lib/test/test_unpack.py` reports OK while running half its tests.
`logging.findCaller`, `warnings` `stacklevel`, and `dataclasses`/`namedtuple`
module resolution are affected by the same gap.

**Deferred deliberately, not overlooked.** The cost is understood and the fix is
affordable whenever it is taken on:

| approach | per call | vs CPython |
|---|---|---|
| today, no frame | 3.42 ns | 7.2x faster |
| eager `PyFrameObject` | 59.88 ns | **2.43x slower** — non-starter |
| `_PyInterpreterFrame`, lazy object | ~8-12 ns (est.) | ~2-3x faster |
| pyc-owned shadow stack | +0.21 ns | free, but invisible to `sys._getframe` |

Eagerly materialising `PyFrameObject` per call would make compiled code slower
than the interpreter it replaces. CPython avoids exactly this: its own 24.68 ns
call already includes a frame, because `_PyInterpreterFrame` is cheap and the
expensive `PyFrameObject` is materialised lazily. So frames do NOT force
anything non-optimizable — they trade some margin, not the win.

The real cost is coupling: `_PyInterpreterFrame` is internal API
(`Py_BUILD_CORE`) whose layout varies with `Py_GIL_DISABLED`, i.e. differs
between the `cp314` and `cp314t` targets I8 treats as separate. Taking it on
requires a layout-conformance check that fails loudly when it shifts.

Frames may NOT be made opt-in: under I2 a default build that diverges
semantically from a `--frames` build is exactly the trade this charter forbids.

A module-level-only trampoline was implemented and reverted (ce52560, reverted):
it fixed `_getframe(0)` but not the depth shift, and it made the doctest failure
*quieter* — turning a raised `ValueError` into a silently empty suite, which
under I1 is the wrong direction.

## 4. Definition of done for v1

- `--static` produces a single self-contained executable, verified by `ldd`.
- A program using a precompiled wheel (NumPy first, PyTorch second) compiles,
  links, and runs.
- CPython `Lib/test/` pass rate published and rising, with no regressions.
- Measurable speedup over CPython on a typed numeric benchmark, achieved
  entirely through provable unboxing, with I2 intact.
- At least two Python targets (e.g. 3.13 and 3.14) build and pass the
  differential suite from one unmodified `pyc` binary.

## 5. Inherited knowledge worth keeping

Port these as *design input*, not as code:

- **Deployment pipeline.** `--static` already worked in the old tree (2.9 MB,
  statically linked, zero deps). Runtime-compiled-to-LLVM-bitcode then LTO'd
  into the user program is the right design — it is what lets a boxed runtime
  get inlined and specialized away. Keep it; it now applies to the C-API shim.
- **Unboxing/specialization learnings.** Native int/float locals, homogeneous
  list layouts (`ilist`/`flist`), native `range` loops, type metadata carried
  on IR instructions. The *empirical knowledge of where the wins are* is real
  and hard-won. It was implemented as ad-hoc fast paths; it must be rebuilt as
  a type lattice and a dataflow analysis (see I3).
- **CPython `ast.parse` as the frontend.** Full grammar for free, forever.
  This was the previous tree's best decision and it survives unchanged.
- CLI shape, opt levels, `--emit-llvm`, `-g`/DWARF.

## 6. Environment (verified 2026-08-22)

- clang/LLVM **22.1.8**.
- CPython **3.14.7** at `/home/joe/local` (`--enable-optimizations
  --enable-shared`) **already ships both libraries**. `libpython3.14.a` (73 MB)
  lives in `LIBPL` — `lib/python3.14/config-3.14-x86_64-linux-gnu/` — which is
  CPython's canonical location for it, not `lib/`. `--with-static-libpython`
  defaults to *yes*, so `--enable-shared` never suppressed it. No rebuild is
  required for the static library. Verified 2026-08-22: linking that `.a` with
  `-rdynamic` produces a working embedded interpreter with no libpython
  dependency.
- A **sysroot is per (version, ABI)**. `cp314` and `cp314t` (free-threaded) are
  distinct ABIs with distinct wheel tags and require separate CPython builds.
- Designate one **primary** target. Its interpreter is the parse oracle and the
  differential oracle (I5); its `Lib/test/` is the metric (I6). Other targets
  must build and pass the differential suite, but need not carry the metric.
