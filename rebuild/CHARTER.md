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
  build lint enforces it. See `INTERFACES.md` §2.3.
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
CPython binary; **any** divergence in stdout, stderr, or exit status fails.

> Previously: 662 inline `(source, expected)` pairs with expected hardcoded as
> "the source of truth". Such a suite stays green forever on a subset. This is
> the mechanism by which the general-purpose intent was lost: every ticket was
> "make this snippet pass", and 662 green cases feel exactly like progress.

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
