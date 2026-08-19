# Wave 11 — Synthetic stdlib fill-out

Operating plan. Same roles as Waves 0–10: Coordinator owns tickets,
tests, docs, git; SWE implements one ticket; SWR is read-only plus
`ISSUES.md`. Nothing starts until you approve the slice.

Goal: finish incomplete **existing** synthetic modules, then add a
small set of high-value **new** synthetic modules. Not “implement
Python 3.14.” CPython has ~297 stdlib names; pyc has 29 synthetics
(`sys` + `syntheticModuleExports()`). Real CPython stdlib source is
still `ImportError`.

Approved first slice: **W11.1** (file leftovers I-223 / I-224 / I-225
Runtime / I-227).

---

## What “done” means

| Outcome | When |
|---|---|
| **fixed** | Binary matches CPython on the ticket cases at `-O0` and `-O2` |
| **wontfix** | Architectural, or already deferred; register says why |
| **accepted** | Design only; implementation is a later ticket |

Hard rules unchanged: one slice, one writer of `Compiler.cpp` /
`Runtime.cpp` / `Codegen.cpp`, tests first, commit each slice, trust
the executable. New export = `syntheticModuleExports()` **and** the
Runtime module dict **and** the ImportError name list in
`Runtime.cpp` (~5148). After Runtime.cpp, rebuild both `libpycrt.a`
and `runtime.bc`. Import suite if a module name is added.

---

## Current inventory (trust the binary)

**Present (29):** `sys`, `re`, `os`/`os.path`, `pathlib`, `subprocess`,
`math`, `cmath`, `json`, `random`, `itertools`, `collections`,
`datetime`, `hashlib`, `base64`, `struct`, `heapq`, `bisect`,
`statistics`, `string`, `textwrap`, `copy`, `uuid`, `functools`,
`operator`, `shutil`, `glob`, `csv`, `decimal`, `time`.

**I-017 children still open:** I-113 µs, I-114 tz, I-115
`datetime⊂date`, I-119 `hashlib.update`, I-120 `getcontext`.
Closed in W10: I-116, I-117, I-118.

**W10 leftovers still open:** I-222 boxed file methods, I-223 `wb`
bytes write, I-224 `readlines` on `"rb"`, I-225 encoding / write-only
read / `readline(n)`, I-226 `rglob`/`**`, I-227 `open(Path)`.

---

## Track A — Finish existing modules (do first)

Small, evidence-backed, no new architecture.

| Slice | IDs | Lock | One-line |
|---|---|---|---|
| **W11.1** | I-223, I-224, I-225 Runtime, I-227 | Runtime | `write(bytes)` in `"wb"`; `readlines` on `"rb"` + closed ValueError; `open(Path)`; write-only `read()` |
| W11.2 | I-222, I-225 Compiler | Compiler + Runtime | Boxed `g(f).read()`; `readline(n)` size. Do **not** register `(2,"read")` without `g_pycFiles` |
| W11.3 | I-119 | Runtime (+ Compiler method arm) | `hashlib` `.update()` incremental |
| W11.4 | I-120 | Runtime + Compiler exports | `decimal.getcontext` / `localcontext` (28-digit, ROUND_HALF_EVEN) |
| W11.5 | new IDs | Runtime + Compiler | `time.time` / `sleep` / `strftime` / `localtime` / `gmtime` (struct_time as tuple) |
| W11.6 | I-226 | Runtime | `Path.rglob` + `glob("**")` + `glob.glob("**")` |
| W11.7 | new IDs | Runtime + Compiler | `os.environ` writes; `os.walk` / `stat` / `chmod`; `os.path` leftovers |
| W11.8 | new IDs | Runtime + Compiler | Fill `math`/`cmath`/`re`/`json`/`random`/`operator`/`uuid` documented holes |

**Defer inside Track A (not this wave unless approved):**

| ID | Why |
|---|---|
| I-113 | Datetime layout change (µs field on tags 14/15) |
| I-114 | `tzinfo` / aware datetimes — large |
| I-115 | Needs type objects (I-011 / I-013). `accepted`, not sneak-coded |
| itertools `count`/`cycle`/unbounded `repeat` | No lazy iterators (eager materialize) |
| `json` custom encoder, `csv` dialects, `struct` native align | Low yield vs size |

---

## Track B — New synthetic modules (after W11.4)

Each module is its own ticket. Subset only. Keep exports in sync.

| Slice | Module | In scope | Out of scope |
|---|---|---|---|
| W11.9 | `fnmatch` | `fnmatch` / `filter` (runtime already has `pyc_fnmatch`) | `translate` |
| W11.10 | `io` | `StringIO` / `BytesIO` | full `IOBase` hierarchy |
| W11.11 | `tempfile` | `mkstemp` / `TemporaryDirectory` / `NamedTemporaryFile` | `SpooledTemporaryFile` |
| W11.12 | `pprint` | `pprint` / `pformat` | `PrettyPrinter` subclassing |
| W11.13 | `errno` + `stat` | common constants + `S_IS*` | platform-all |
| W11.14 | `shlex` / `filecmp` | `split`; `cmp` / `cmpfiles` | `shlex` class |
| W11.15 | `hmac` / `secrets` | `hmac.new`+`.hexdigest`; `token_hex`/`token_bytes` | compare-digest timing claims |
| W11.16 | `tomllib` | `loads` / `load` | `dump` |
| W11.17 | `fractions` | `Fraction` arithmetic | `limit_denominator` edge cases |
| W11.18 | `argparse` | `ArgumentParser` + `add_argument` + `parse_args` on `sys.argv` | subparsers, groups |
| W11.19 | `gzip` / `zlib` | compress/decompress + `GzipFile` read | incremental streams |
| W11.20 | `warnings` / `traceback` | `warn`; `format_exception` using existing frames | filters registry |

---

## Track C — wontfix this wave

`asyncio`, `threading`, `multiprocessing`, `socket`/`ssl`/`select`,
`http`/`email`/`urllib.request`, `xml`/`sqlite3`/`ctypes`,
`tkinter`, `typing`/`dataclasses`/`enum` (need type objects),
`pickle`, `importlib` internals, `unittest`/`doctest` as a runner,
`inspect`/`ast`/`dis`, `zoneinfo`, `memoryview`, `exec`/`eval`.

---

## Cadence (every slice)

1. Coordinator writes ticket (template in `agents/coordinator.md`) and
   failing CASES under a unique `# W11.N / I-NNN` banner. Prove they
   fail on the parent commit.
2. Launch SWE (named file lock) and SWR in parallel.
3. Rebuild. `PYC_BINARY=./build/pyc python3 tests/runner.py`.
4. New/changed import → `./test/import_tests/run_import_tests.sh`.
   Always `-O2` smoke. File I/O / hashlib / decimal → valgrind on the
   FILE_CASE.
5. Blocking SWR → back to SWE. Non-blocking → ISSUES.md, merge, update
   FEATURES.md.

---

## W11.1 ticket (approved)

```
Title: W11.1 — file leftovers (I-223, I-224, I-225 Runtime, I-227)
Goal: Match CPython on binary write, binary readlines, closed
  readlines, open(Path), and read() on a write-only handle.
In scope:
  - pyc_file_write_adapter: type-17/18 bytes in "wb"; TypeError for
    str on "wb" and bytes on text mode
  - PyBuiltin_FileReadlines: "rb" → list[bytes]; closed → ValueError
  - PyBuiltin_Open: accept type-16 Path (str from path payload)
  - read() on write-only → UnsupportedOperation (or IOError-shaped)
Out of scope:
  - I-222 boxed g(f).read() / bound f.read
  - I-225 encoding= and compile-time readline(n) (W11.2)
  - I-226 rglob
Files SWE may edit:
  src/runtime/Runtime.cpp, include/pyc/runtime.h
  (Codegen.cpp only if a new extern is required)
Files SWE must not edit:
  src/Compiler.cpp, tests/runner.py, FEATURES.md, ISSUES.md
Tests Coordinator already added: (insert before SWE launch)
Verify:
  - PYC_BINARY=./build/pyc python3 tests/runner.py
  - PYC_BINARY=./build/pyc python3 tests/o2_smoke.py
  - valgrind --tool=memcheck on the W11.1 FILE_CASE
CPython reference: 3.14 file objects / PathLike
Related ISSUES.md ids: I-223 I-224 I-225 I-227 (parent I-118)
```

---

## Order vs locks

```
W11.1 Runtime ──► W11.2 Compiler+Runtime
                 ──► W11.3 hashlib (Runtime)
                 ──► W11.4 decimal exports
                 ──► W11.5 time
                 ──► W11.6 glob **
                 ──► W11.7 os
                 ──► W11.8 fill-ins
                 ──► W11.9+ new modules (serial; each adds exports)
```

W11.2 must not overlap any other Compiler writer.

---

## Approval

W11.1 is approved. Later slices wait for an explicit go.
