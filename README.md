# pyc

An AOT compiler for Python. Parses with the target CPython, lowers a generated
typed AST to LLVM IR, and links a native executable against that CPython —
same object model, real stdlib, precompiled C-extension wheels.

Binding invariants: [rebuild/CHARTER.md](rebuild/CHARTER.md).
Why the previous tree was discarded: [rebuild/ARCHITECTURE_REVIEW.md](rebuild/ARCHITECTURE_REVIEW.md).

## Status

Measured by `./verify/measure_run.py` against CPython 3.14.7. Every case is
compared at run time; no expected output is stored ([CHARTER I5](rebuild/CHARTER.md)).

| Corpus | Pass rate | |
|---|---|---|
| CPython `Lib/test/` | **63.24%** | 246/389 files |
| language corpus | **98.73%** | 777/787 cases |

`Lib/test` is the north-star metric (CHARTER I6). It may not regress.

Python frames (C1a): compiled functions and the module body now push a
`PyFrameObject`, so `locals()` / `globals()` / `eval` / `exec` match CPython
on the known-gaps probes. Class-body `locals()` is still the enclosing
module (one leftover). Eager frames are the expensive path; C1b will
replace them with `_PyInterpreterFrame`. Plan:
[rebuild/CORRECTNESS.md](rebuild/CORRECTNESS.md).

## Build

Needs clang++/LLVM 22 and a CPython **sysroot** (Tier 1: static libpython,
dynamic libc, `-rdynamic`).

```bash
./tools/build-python-sysroot.sh --version 3.14.7 --jobs "$(nproc)"
make -C compiler                          # writes /tmp/pyc_lower
export PYC_SYSROOT=$HOME/opt/py-sysroots/cp314-3.14.7-tier1
```

## Usage

```bash
./compiler/tools/pycc hello.py -o hello          # default -O1 (LLVM backend)
./compiler/tools/pycc hello.py -o hello -O0
./compiler/tools/pycc hello.py --emit-llvm -o hello.ll
./hello
```

`pycc` produces a Tier-1 binary: no libpython `DT_NEEDED` entry, `dlopen` still
works, so NumPy/PyTorch wheels load. Fully static (Tier 2) is deferred.

## Test

```bash
make -C verify fast      # language corpus, fail on a new silent wrong answer
make -C verify verify    # PR-equivalent gate + monotonicity vs baseline
make -C verify metric    # Lib/test — the published number
```

Oracle and subject must share the sysroot (`PYC_SYSROOT` or the default above).

## Layout

```
compiler/     frontend, lowering, codegen, C-API binding, pycc
verify/       differential harness and corpora
rebuild/      CHARTER, interfaces, agent directives, correctness plan
tools/        sysroot builder, nightly checker
```

Flow: target `ast.parse` → JSON → generated `pyc::ast` → SSA IR → LLVM IR →
`clang++` + static libpython → native executable.

## Documentation

- [rebuild/CHARTER.md](rebuild/CHARTER.md) — product definition and invariants
- [rebuild/INTERFACES.md](rebuild/INTERFACES.md) — layer contracts
- [rebuild/AGENT_DIRECTIVES.md](rebuild/AGENT_DIRECTIVES.md) — how to work on it
- [rebuild/CORRECTNESS.md](rebuild/CORRECTNESS.md) — open correctness work
- [rebuild/UNBOXING.md](rebuild/UNBOXING.md) — performance (after correctness)
- [compiler/README.md](compiler/README.md) — frontend / C-API notes
- [verify/README.md](verify/README.md) — harness semantics
- [AGENTS.md](AGENTS.md) — build/test for coding agents

## License

Apache License 2.0 — see [LICENSE](LICENSE).
