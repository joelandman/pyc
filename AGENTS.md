# AGENTS.md

Compact guide for OpenCode sessions working on **pyc**, an AOT compiler for a
Python subset (Python source → AST → IR → LLVM IR → native executable via a
boxed `PyObject*` runtime with refcounting). C++20, Clang++.
LLVM via `find_package(LLVM)` (docs historically said 18; this tree
currently configures against LLVM 22 on the development machine).

## Build & test

```bash
mkdir -p build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j$(nproc)
make -C build check          # runner + import suite + -O2 smoke; exit code is real
```

Dependencies: LLVM (`find_package(LLVM)`; 18 historically, 22 on this machine), `libmpdec-dev` (mpdecimal, backs `decimal.Decimal`),
PCRE2 (backs `re`), Python3 dev headers. The build hard-codes
`clang++` as the C++ compiler (CMakeLists.txt:24).

Single-test / focused verification (use these, not `make check`, when iterating):

```bash
PYC_BINARY=./build/pyc python3 tests/runner.py                  # full suite
./build/pyc tests/nbody.py -o /tmp/t.bin -O0 && /tmp/t.bin 100  # one FILE_CASE
./build/pyc tests/hello.py --emit-llvm -o /tmp/x && head /tmp/x.ll  # inspect IR
./test/import_tests/run_import_tests.sh                         # import system suite (also in `make check`)
PYC_BINARY=./build/pyc python3 tests/o2_smoke.py                # hello.py + nbody.py 100 at -O2
```

`build.sh` wraps configure+build+test; `--clean` wipes `build/`, `--install PREFIX` installs.

## Test suite semantics (easy to misread)

`tests/runner.py` has **two** sections, and they fail differently:

- `CASES` (lines 7–3619): 597 inline source/expected pairs. Compiled at
  **`-O0`** and compared against CPython output. The hardcoded `expected` string
  is the source of truth; python3 is only a sanity check. The runner exits 0
  only if `ok==total`. `make check` no longer swallows that exit (`|| true`
  was removed in Wave 0).
- `FILE_CASES` (lines 3620–3661): 29 real `.py` programs in `tests/`, each compiled
  at `-O0` and compared to CPython. **A mismatch is a real regression**: the
  runner prints a `DIFF` block and exits 1.

A third check runs after both sections: `tests/check_dispatch_chain.py`,
a static guard against unreachable arms in `Compiler.cpp`'s
`lowerMethodCall` (a catch-all arm makes any later same-name arm dead —
how `Counter.update` shipped a runtime function that never ran). It is
counted in the totals and treated like a FILE_CASE, so a violation fails
the run. It prints its documented exemptions on every run; read them
rather than adding new ones reflexively.

When adding a feature, add to `CASES` for inline coverage and to `FILE_CASES`
+ a real file for end-to-end coverage. `modifiers.py` is included (the `-O0`
continue bug is fixed). `mbs.py` is excluded because it exceeds the 5s
timeout — `time.perf_counter` itself is supported. Read the comments before
re-enabling anything.

The runner auto-discovers the binary via `PYC_BINARY` env, then `./pyc`,
`./build/pyc`, etc. (runner.py:3667–3685). The 5s per-command `timeout` in
`run()` (runner.py:3663) bites slow programs.

## Architecture / where things live

The **build only compiles files under `src/`** plus `runtime/b7_import.cpp`.
CMakeLists.txt:62–69 lists the exact compiler sources:

- `src/main.cpp` — CLI entrypoint, flag parsing.
- `src/frontend/PythonParser.cpp` — uses the **Python C API (`ast.parse`)** to
  parse. pyc is NOT a from-scratch Python parser; it shells out to libpython.
- `src/Compiler.cpp` — **~11k lines, the lowering core.** `LoweringVisitor`
  walks the Python AST and emits `ModuleIR`. Most feature work happens here.
- `src/ir/IR.cpp`, `src/ir/LLVMDCE.cpp` — IR data + a dead-code-elimination pass.
- `src/codegen/Codegen.cpp` — IR → LLVM IR (~3.6k lines). Owns the
  `__apply__N` indirect-call adapter generation and the native/local
  fast-path gating (see gotchas below).
- `src/runtime/Runtime.cpp` — **~11.5k lines, the boxed runtime.** Linked into
  every compiled binary. No CPython dependency at runtime. Built twice: once
  as a static lib (`pyc_runtime` → `libpycrt.a`) and once as **LLVM bitcode**
  (`build/runtime.bc`) for LTO into user programs. The bitcode path is
  baked into the compiler via `PYC_RUNTIME_BC` / `PYC_SOURCE_DIR` defines
  (CMakeLists.txt:73); do not relocate `build/runtime.bc` without rebuilding.

**Stale-looking but NOT dead:** `runtime/b7_import.cpp` is in the build
(CMakeLists.txt:85). The root-level `codegen/`, `frontend/`, `ir/` directories
and the `test/` tree are legacy/standalone and **not compiled by the main
build** — ignore them when editing the compiler; the live sources are under
`src/`. Public headers live in `include/pyc/`; runtime-internal headers in
`runtime/`.

Flow: `src/main.cpp` → `Compiler` → `PythonParser` (libpython AST) →
`LoweringVisitor` (Compiler.cpp) → `ModuleIR` (IR.h) → `Codegen::generate`
(Codegen.cpp) → `llvm::Module` → LLVM passes (O0–O3) → object →
`clang++` links `libpycrt`/bitcode → executable. See README.md:85 for the
diagram and IMPLEMENTATION.md for design rationale.

## Optimization levels (non-obvious)

`-O0` means **no runtime-bitcode LTO and no LLVM passes** (raw IR, for
debugging). `-O1..3` add LLVM passes **plus LTO of `runtime.bc`**. `-O2` is
the default. `-O4`/`-O5` (PGO/ThinLTO/multi-versioning) exist in `--help` but
are advanced/experimental — don't assume they're exercised by the test suite,
which runs everything at `-O0`.

The runner compiles FILE_CASES at **`-O0`**, so a program that only works at
`-O2` will show as a DIFF. `make check` also runs `tests/o2_smoke.py` so an
LTO-only regression cannot hide completely. Verify both levels on feature work.

## Conventions & gotchas

- **Compiler is C++20, force-built with `clang++`** (not g++). CMake exports
  `compile_commands.json` in `build/` for tooling.
- **No function-local C++ constructors in `Runtime.cpp` (I-049).**
  `runtime.bc` LTO (`-O2`) does not run compiler-emitted ctors for
  function-local / function-static objects. A `static std::unordered_map`
  inside a helper is a zeroed object and SEGVs. Heap-allocate behind a BSS
  pointer (`new[]`) or use a file-scope `thread_local`. `libpycrt.a` (`-O0`)
  does run those ctors, so the bug is `-O2`-only.
- **Boxed-everything by default.** Most values are `PyObject*`; a value's
  type is carried in the box, not in LLVM types. `Codegen.cpp` has
  "native-local" fast paths for `int`/`float` that bypass boxing — these are
  gated on whether the target is a *declared module global* vs a local. Mis-
  gating here has historically caused null derefs on module-level float
  globals (see FIXES.md "Pyc_Apply Dispatch for main()" note). When touching
  the `"assign"` handler in Codegen.cpp, re-check that gating.
- **Indirect calls** (lambdas-as-values, closures, decorators) go through
  `Pyc_Apply` + per-function `__apply__N` adapters generated in Codegen.cpp.
  The adapter's parameter-shape analysis must distinguish `*args` from
  `**kwargs` (stored internally as `"**kwargs"`, i.e. *two* leading stars).
  Several past crashes came from "first star-prefixed name wins" bugs here
  (see IMPLEMENTATION.md's `**kwargs` section). If you touch adapter
  generation, handle both slots.
- **`exec()`/`eval()` are intentionally unsupported.** Don't try to add them.
  Real CPython stdlib modules beyond the synthetic ones (`sys`, `re`, `os`,
  `subprocess`, `functools`, `cmath`, `time.perf_counter`, `math`, `json`,
  `random`, `itertools`, `collections`, `datetime`, `hashlib`, `base64`,
  `struct`, `heapq`, `bisect`, `statistics`, `string`, `textwrap`, `copy`,
  `uuid`, `operator`, `shutil`, `glob`, `csv`, `decimal`, `pathlib`,
  `re`/PCRE2) cannot be imported — pyc reports a clear `ImportError` rather
  than attempting to compile CPython source. Synthetic module exports are
  hand-maintained in `syntheticModuleExports()` (Compiler.cpp); keep that
  table in sync when adding to a synthetic module's dict in Runtime.cpp.
- **Top-level `__name__` is `"__main__"`**, not `"__module__"`. (Historic bug,
  documented in FIXES.md.)
- **Generated artifacts in the repo root** (`a.out`, `a.out.ll`, `a.out.o`,
  `*_opt*.o`, `*_b7_modules.c`, `*.ll`, `*.s`) are gitignored output from
  direct `pyc` runs. Don't commit them; don't treat them as sources. The
  `*_b7_modules.c` files are generated alongside each compiled binary for the
  module registry.
- **`build/`, `build_debug/`, `build_asan/`** are all gitignored — use any of
  these for out-of-source builds. ASAN builds: configure with
  `-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"` and link
  the runtime accordingly.
- **No formal lint/typecheck step.** There's no `.clang-tidy`/CI workflow in
  the repo; verification = `make -C build check` (runner + import suite +
  `-O2` smoke). Trust the runner, not prose.
- **Doc files are large and partially historical.** README.md (build/usage),
  FEATURES.md (capability list), ISSUES.md (living register),
  IMPLEMENTATION.md (design + a long log of past bug hunts), FIXES.md,
  DEBUGGING_PLAN.md, OPTIMIZATION_PLAN.md, PERFORMANCE_*.md exist.
  IMPLEMENTATION.md is the most useful for "why is X like this" after you
  read its Current Status header. When docs conflict with CMakeLists.txt or
  the runner, trust the executable.

## Multi-agent roles

See `agents/swe.md`, `agents/swr.md`, `agents/coordinator.md`.

- **SWE** implements one ticket. Exclusive lock on `Compiler.cpp` /
  `Runtime.cpp` / `Codegen.cpp` as named. Does not edit ISSUES.md or
  `tests/runner.py` unless the ticket says so.
- **SWR** is read-only on sources. Owns `ISSUES.md`. Classifies findings;
  only crash/wrong-answer on this slice blocks merge.
- **Coordinator** writes tickets and failing tests, runs harnesses, updates
  docs, merges. Never two writers on the same lock file.

Do not edit the legacy OpenCode stubs `agents/coding.md` / `review.md` /
`debug.md` as if they were the source of truth — they redirect here.

## Adding a feature: checklist

1. Lower it in `src/Compiler.cpp` (`LoweringVisitor`).
2. Emit it in `src/codegen/Codegen.cpp` (and any runtime helper in
   `src/runtime/Runtime.cpp`).
3. If it's a new builtin/module export, update `syntheticModuleExports()` in
   Compiler.cpp to match.
4. Add inline cases to `CASES` and a real file + `FILE_CASES` entry in
   `tests/runner.py`.
5. `make -C build -j && make -C build check`; if touching imports, also run
   `./test/import_tests/run_import_tests.sh`.
6. Verify at `-O0` (what the runner uses) AND `-O2` (default for users) —
   behavior can diverge across opt levels.