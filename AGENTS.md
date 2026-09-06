# AGENTS.md

pyc on **devel**. The old boxed runtime (`src/`, `tests/runner.py`) is gone.
Read [rebuild/CHARTER.md](rebuild/CHARTER.md) before the first edit; it is binding.

Priority when they conflict: **correctness, then completeness, then performance.**
A faster program that disagrees with CPython is a wrong program.

## Build & test

```bash
make -C compiler                         # /tmp/pyc_lower
export PYC_SYSROOT=$HOME/opt/py-sysroots/cp314-3.14.7-tier1
export PYC_LOWER=/tmp/pyc_lower
./compiler/tools/pycc prog.py -o /tmp/p -O0
make -C verify fast                      # inner loop
make -C verify verify                    # PR gate
```

Toolchain: clang++/LLVM 22. Sysroot: `./tools/build-python-sysroot.sh`.
CI compile line is in `.github/actions/setup-pyc/action.yml`.

## Invariants that bite

- **I1.** Unsupported construct → compile error naming construct, line, reason.
  Never a wrong value. A refusal is a hypothesis (I1a), not a place to stop.
- **I3.** Protocols, never callsite dispatch chains on method names.
- **I5.** No hardcoded expected output. Oracle is the sysroot CPython.
- **I6.** `Lib/test` pass rate is published and may not regress.
- **I9.** A claim about this compiler is measured, or it is not made.

## Where to edit

| Layer | Paths | Do not |
|---|---|---|
| A1 frontend | `compiler/pyc_parse/`, `compiler/tools/gen_ast.py`, `compiler/include/pyc/ast/` | hand-edit `generated.hpp` |
| A2 runtime | `compiler/src/rt/`, `compiler/include/pyc/rt/` | define a `PyObject` layout |
| A3 lowering | `compiler/src/lower.cpp`, `scope.cpp`, `ir.cpp` | generic `auto` visitor arms |
| A4 backend | `compiler/src/codegen.cpp` | |
| A5 verify | `verify/` | store an expected string |

Lock files (one writer): `compiler/src/lower.cpp`, `compiler/src/codegen.cpp`,
`compiler/src/rt/support.cpp`.

## Agent roles

Contracts: [rebuild/AGENT_DIRECTIVES.md](rebuild/AGENT_DIRECTIVES.md).
Correctness slices: [rebuild/CORRECTNESS.md](rebuild/CORRECTNESS.md).

When a doc disagrees with `compiler/tools/pycc` or a `verify/` run, trust the
executable and fix the doc.
