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
| CPython `Lib/test/` | **94.34%** | 367/389 files, measured 2026-10-28 |
| language + gaps + concurrency | **100%** | 886/886 cases |

`Lib/test` is the north-star metric (CHARTER I6). It may not regress.
The fraction above is the last `make -C verify metric` record. Commits
after 2026-10-28 are not in it until that command is run again.

Python frames (C1b): compiled functions, the module body, and class bodies
push `_PyInterpreterFrame` on the thread datastack, so `locals()` /
`globals()` / `eval` / `exec` / `sys._getframe` match CPython on the
known-gaps probes. Plan: [rebuild/CORRECTNESS.md](rebuild/CORRECTNESS.md).

Current state, MVP checklist, and sysroot-independence plan:
[rebuild/STATUS.md](rebuild/STATUS.md).

## Build

Needs clang++/LLVM 22 and a CPython **sysroot** (Tier 1: static libpython,
dynamic libc, `-rdynamic`). Prefer a prebuilt tarball (S2). Compiling
CPython is how the tarball is *produced*, not how you onboard.

Two published tarballs (linux x86_64):

| Asset | Tag | What |
|---|---|---|
| `pyc-linux-x86_64.tar.xz` | `pyc-linux-x86_64` | `pycc`, `pyc_lower`, `lib/pyc` |
| `cp314-3.14.7-tier1-x86_64.tar.xz` | `sysroot-cp314-linux-x86_64` | CPython sysroot |

The machine that *runs* `pycc` still needs clang++/LLVM 22 (it links the user program). The sysroot is not inside the compiler tarball.

```bash
mkdir -p "$HOME/opt/pyc"
tar -C "$HOME/opt/pyc" -xJf pyc-linux-x86_64.tar.xz
export PATH="$HOME/opt/pyc/bin:$PATH"
pycc --fetch-sysroot
pycc hello.py -o hello
```

From a clone:

```bash
./tools/install-pyc.sh --prefix "$HOME/opt/pyc"
export PATH="$HOME/opt/pyc/bin:$PATH"
pycc --fetch-sysroot          # into prefix/sysroots/cp314-3.14.7-tier1
pycc hello.py -o hello
pycc --python=3.14 hello.py -o hello   # selects that sysroot artifact
pycc --list-python-targets
./tools/pack-pyc.sh -o /tmp/pyc-linux-x86_64.tar.xz
```

Layout: `bin/pycc`, `bin/pyc_lower`, `lib/pyc/`, `sysroots/<abi>-<version>-tier1/`.
`--python=X.Y` picks a tree under `sysroots/`; a missing version is an error,
not a silent fallback. A second CPython is another directory there, not a
second compiler.

From a clone (how `pycc` is **built** without a prefix):

```bash
./compiler/tools/pycc --fetch-sysroot
./compiler/tools/pycc hello.py -o hello
# or: ./tools/setup-machine.sh --beside
```

`--fetch-sysroot` installs the S2 tarball next to `pycc`. After that, no
`PYC_SYSROOT` is required (`--python-sysroot` and the env var still override).
Missing sysroot is a compile error, not PATH `python3`. `PYC_FETCH=1` fetches
on first compile.

If the GitHub Release asset is missing, or you are changing A2 headers,
`build-python-sysroot.sh` is how the tarball is *produced*:

```bash
./tools/build-python-sysroot.sh --version 3.14.7 --jobs "$(nproc)"
```

## Usage

```bash
pycc hello.py -o hello          # default -O1 (LLVM backend)
pycc hello.py -o hello -O0
pycc hello.py --emit-llvm -o hello.ll
./hello
```

`pycc` produces a Tier-1 binary: no libpython `DT_NEEDED` entry, `dlopen` still
works, so NumPy/PyTorch wheels load. Fully static (Tier 2) is deferred.

### Deploying a binary

The binary embeds the absolute path of the sysroot it was compiled against.
It finds its standard library in this order:

1. `PYTHONHOME`, if set;
2. `lib/python3.14` next to the executable (or its parent directory);
3. the compile-time sysroot.

A portable deployment is the binary plus that folder:

```
app/
  myprog                # compiled binary
  lib/python3.14/       # from the sysroot
```

On a machine where none of the three exists the binary exits with a
diagnostic naming what it searched and the fix, rather than CPython's bare
`Failed to import encodings module`.

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
- [rebuild/STATUS.md](rebuild/STATUS.md) — current state, MVP, next steps
- [rebuild/CORRECTNESS.md](rebuild/CORRECTNESS.md) — open correctness work
- [rebuild/UNBOXING.md](rebuild/UNBOXING.md) — performance (after correctness)
- [compiler/README.md](compiler/README.md) — frontend / C-API notes
- [verify/README.md](verify/README.md) — harness semantics
- [AGENTS.md](AGENTS.md) — build/test for coding agents

## License

Apache License 2.0 — see [LICENSE](LICENSE).
