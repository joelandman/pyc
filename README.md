# pyc

An AOT (Ahead-Of-Time) compiler for a substantial Python subset. Parses Python
source using the Python C API, lowers the AST through a visitor-based IR, generates
LLVM IR, optimizes it, and produces standalone native executables via a minimal
`PyObject*`-based boxed runtime with refcounting.

Written in C++ with Clang++ and LLVM (`find_package(LLVM)`; 18 historically,
22 on the current development machine). No C/C++ intermediate language for
the normal compiler path.

## Status

The project is being rebuilt on CPython's object model. See
[rebuild/CHARTER.md](rebuild/CHARTER.md) for the binding invariants and
[rebuild/ARCHITECTURE_REVIEW.md](rebuild/ARCHITECTURE_REVIEW.md) for why.

| Corpus | Pass rate | |
|---|---|---|
| CPython `Lib/test/` | **5.66%** | 22/389 files |
| language corpus | **99.04%** | 721/728 cases |

`Lib/test` is the north-star metric (CHARTER I6): the pass rate over CPython's
own test suite, published low and honest, and never allowed to regress. Both
numbers are `matched / corpus size` — the denominator is the corpus, so they
move only when the compiler does. Every case is compared against a real CPython
at run time; no expected output is stored anywhere ([CHARTER I5](rebuild/CHARTER.md)).

**Zero P0 silent wrong answers in the language corpus.** `Lib/test` currently has
**one** — `test_unpack.py`, where a compiled binary runs 1 test instead of 2 and
reports OK because `__main__` has no `__file__`, so `doctest.DocTestSuite()`
fails and the failing doctest never runs. It is tracked as issue #9 and outranks
the pass rate: failing loudly is the property the rebuild exists to protect.

Measured by `./verify/run.py`; gated per-commit in CI
(`.github/workflows/verify.yml`) and nightly (`metric.yml`).

> The rest of this file still describes the **previous** tree (`src/`,
> `tests/runner.py`, the boxed `PyObject*` runtime). It has not been rewritten
> for the new tree under `compiler/`.

## Build

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Usage

```bash
pyc hello.py -o hello          # compile
pyc hello.py -o hello --static # fully static binary
pyc hello.py --emit-llvm       # dump LLVM IR
pyc hello.py -O2 -o hello      # with O2 optimisation
./hello
```

### Command-Line Options

| Flag | Description |
|------|-------------|
| `-o output` | Output file path (default: `a.out`) |
| `--static` | Produce fully static binary (no dynamic libs) |
| `-O0` | True O0: **no** runtime bitcode LTO, **no** LLVM passes (debug / raw IR) |
| `-O1` | LLVM O1 + runtime bitcode LTO |
| `-O2` | LLVM O2 + runtime bitcode LTO (default) |
| `-O3` | LLVM O3 + runtime bitcode LTO |
| `--opt=N` | Alias for `-ON` (deprecated) |
| `--emit-llvm` | Emit LLVM IR to `output.ll` instead of binary |
| `--emit-asm` / `-S` | Emit assembly to `output.s` instead of binary |
| `-g` | Emit DWARF debug info (source line mapping for gdb/lldb). Use with `-O0`. Pretty-print `PyObject*` via `source tools/pyc_gdb.py` in gdb. |
| `--verbose` | Print verbose compilation information |

## Testing

```bash
cd build && make check   # or: ctest
```

`make check` runs `tests/runner.py` (inline `CASES` + `FILE_CASES` + dispatch-chain
check, all at `-O0`), the import suite, and a thin `-O2` smoke (`tests/o2_smoke.py`).
Its exit code is the suite's exit code — a green `make check` means those steps
passed. For a single focused run:

```bash
PYC_BINARY=./build/pyc python3 tests/runner.py
./test/import_tests/run_import_tests.sh
PYC_BINARY=./build/pyc python3 tests/o2_smoke.py
```

The runner compiles each case and compares output against CPython. File-based
programs in `tests/` cover optimization-sensitive behavior such as native
`range` loops, numeric locals, numeric list mutation and aliasing, command-line
arguments, default arguments, nested destructuring, `tests/modifiers.py`, and
`tests/nbody.py`. `tests/mbs.py` is excluded because it exceeds the runner's
5s per-command timeout (`time.perf_counter` itself is supported).

## What Compiles Today

See [FEATURES.md](FEATURES.md) for a complete list of supported features.

### Quick Summary

- **Types**: int, float, bool, str, list, dict, tuple, set, None, complex, bytes, bytearray, decimal.Decimal
- **Operators**: `+ - * / // % **`, `== != < > <= >=`, `is`, `in`, `and`, `or`, `not`, unary `-`
- **Control flow**: `if/elif/else`, `while`, `for`, `break`, `continue`, ternary
- **Functions**: `def`, `lambda`, nested functions, closures (`nonlocal`), decorators
- **Classes**: `class`, `__init__`, inheritance, `super()`, `__str__`/`__repr__`
- **Exceptions**: `try/except/finally/else`, structured exceptions, exception classes
- **Statements**: `with`, `match/case`, `assert`, `del`, walrus `:=`, `import`
- **Builtins**: `print`, `range`, `len`, `str`, `int`, `float`, `complex`, `abs`, `min`, `max`, `list`, `enumerate`, `zip`, `sum`, `sorted`, `any`, `all`, `isinstance`, `bool`, `type`, `id`, `repr`, `hex`, `oct`, `bin`, `ord`, `chr`, `round`, `divmod`, `pow`, `reversed`, `cmp_to_key`
- **Standard library stubs**: `re` (PCRE2-backed: search/match/finditer/findall/sub/split/compile, IGNORECASE/MULTILINE/DOTALL flags), `os` (path helpers, getcwd, listdir, makedirs, environ), `pathlib` (`Path`), `subprocess` (call, check_output), `sys`, `cmath`, `math` (~25 functions wrapping libm), `json` (dumps/loads), `random` (CPython-exact MT19937), `itertools` (chain/product/combinations/permutations/starmap/islice/zip_longest/accumulate/takewhile/dropwhile/compress/groupby/chain.from_iterable), `collections` (Counter incl. `most_common`/`elements`/`subtract`/`update`, deque, namedtuple, defaultdict), `datetime` (`date`/`datetime`/`timedelta`), `hashlib` (md5/sha1/sha256, from scratch), `base64`, `struct`, `heapq`, `bisect`, `statistics`, `string`, `textwrap`, `copy`, `uuid`, `functools` (reduce/partial/wraps/lru_cache), `operator`, `shutil`, `glob`, `csv`, `decimal` (`Decimal`, arbitrary precision via libmpdec) — see FEATURES.md

## Architecture

```
Python source
    │  Python C API (ast.parse)
    ▼
ASTNode tree  (PythonParser.cpp)
    │  LoweringVisitor
    ▼
ModuleIR  (IR.h / IR.cpp)
    │  Codegen::generate
    ▼
llvm::Module  (Codegen.cpp)
    │  LLVM passes (O0–O3)
    ▼
.o object file
    │  clang++ + Runtime.cpp
    ▼
native executable
```

**Runtime** (`src/runtime/Runtime.cpp`): standalone C++ file, no CPython
dependency. Provides `PyObject` (flat struct: `refcount`, `type`, `value`/`dvalue`/
`list`/`dict`/`str`/`cell_content`), refcounting, arithmetic, comparison, print,
and all builtins. Types: int, list, dict, str, float, bool/None, cell, super
proxy, compiled regex, match object, exception, function, exception class,
complex, date/datetime, timedelta, pathlib.Path.
Exceptions use setjmp/longjmp frames. Callables dispatch through a registry of
`__apply__` adapters (`Pyc_Apply`). Linked into every compiled binary.

**IR**: linear instruction list per function. Instructions: `const`, `fconst`,
`bconst`, `nconst`, `assign`, `add`/`sub`/`mul`/`div`/`truediv`/`mod`/`pow`,
`icmp`, `br`, `label`, `call`, `ret`. IR instructions carry conservative result
type metadata (`int`, `float`, `bool`, `str`, or `boxed`).

## Documentation

- [README.md](README.md) — This file: build, usage, options, quick feature summary
- [FEATURES.md](FEATURES.md) — Capability list (what compiles today)
- [ISSUES.md](ISSUES.md) — Open bugs, latent issues, and closed-but-don't-reopen items
- [IMPLEMENTATION.md](IMPLEMENTATION.md) — Design choices and a historical log of bug hunts
- [KnownGapsPlan.md](KnownGapsPlan.md) — Gaps 1–4 (all fixed)
- [AGENTS.md](AGENTS.md) — Build/test gotchas; SWE / SWR / Coordinator roles
- [DEBUGGING_PLAN.md](DEBUGGING_PLAN.md) — `-g` DWARF (implemented; leftovers in ISSUES I-019)

When a doc disagrees with `tests/runner.py` or the `pyc` binary, trust the executable.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

## Development history

Initially scaffolded with Grok (xAI). Extended substantially with Claude (Anthropic).
See [GROK.md](GROK.md) for early history.
