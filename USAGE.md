# USAGE — installing and using pyc

Step-by-step for the three things you install, the environment variables,
and what every command does. Everything here is the behaviour of the
committed tools; where a number matters it was measured, not assumed.

```
 pyc (the compiler)          CPython sysroot (the target)        host toolchain
 one relocatable binary      static libpython.a + headers +      clang++/LLVM 22
 parses with the target,     stdlib + the interpreter itself     (links the user
 links your program          (used as parse oracle and as the   program; the sysroot
 against libpython.a         source of libpython.a)              is NOT in it)
```

Two published tarballs (linux x86_64, GitHub Releases):

| Asset | Release tag | What it is |
|---|---|---|
| `pyc-linux-x86_64.tar.xz` | `pyc-linux-x86_64` | the compiler prefix |
| `cp314-3.14.7-tier1-x86_64.tar.xz` | `sysroot-cp314-linux-x86_64` | the CPython 3.14.7 sysroot |

---

## 1. Install

### 1.1 Host requirements

The machine that **runs `pyc`** needs (the sysroot is a tarball, not a build):

- `clang++`/LLVM **22** — it compiles the runtime objects and links your
  program. On Debian/Ubuntu: `https://apt.llvm.org` (`clang++-22`, `ld.lld`).
- `make`, `curl`, `tar`, `xz` — for the install scripts and `--fetch-sysroot`.
- A glibc at least as new as the sysroot's build host (the sysroot manifest
  records it under `"glibc"`; the published one is built on ubuntu-24.04).

Compiling CPython (its build deps: zlib, ssl, ffi, bzip2, lzma, sqlite3,
readline, ncurses, uuid) is only needed if you build the sysroot yourself
(§1.4). `./tools/build-python-sysroot.sh --check-deps` lists exactly what is
missing.

### 1.2 From the release tarball (recommended)

```bash
# 1. the compiler
mkdir -p "$HOME/opt/pyc"
curl -fL -o /tmp/pyc.tar.xz \
  https://github.com/joelandman/pyc/releases/download/pyc-linux-x86_64/pyc-linux-x86_64.tar.xz
tar -C "$HOME/opt/pyc" -xJf /tmp/pyc.tar.xz
export PATH="$HOME/opt/pyc/bin:$PATH"

# 2. the sysroot: pyc downloads and installs it itself
pyc --fetch-sysroot
#   -> pyc: downloading https://github.com/joelandman/pyc/releases/download/sysroot-cp314-linux-x86_64/cp314-3.14.7-tier1-x86_64.tar.xz
#      pyc: sysroot installed at /home/<you>/opt/pyc/sysroots/cp314-3.14.7-tier1

# 3. compile and run
cat > hello.py <<'EOF'
def f(n):
    return sum(i * i for i in range(n))

print(f(1000))
EOF
pyc hello.py -o hello        # default -O1
./hello
#   332833500
```

Check the result is what the CHARTER promises (Tier 1: libpython is inside
the binary, no `DT_NEEDED` entry for it, and `dlopen` still works):

```bash
ldd hello | grep -i libpython    # -> (nothing)
pyc --list-python-targets        # -> the sysroots pyc can see (see §3.2)
```

### 1.3 From a clone (developers, compiler contributors)

```bash
git clone https://github.com/joelandman/pyc && cd pyc

# one command: builds pyc and installs the compiler and sysroot trees into
# one self-contained prefix, then runs a differential smoke (the pyc side
# vs the sysroot's own CPython — I5)
./install.sh --prefix "$HOME/opt/pyc"
./install.sh --prefix DIR --dryrun       # print the plan, change nothing
./install.sh --uninstall --prefix DIR    # remove only the paths pyc owns
```

The prefix is self-contained after this: `$PREFIX/bin/pyc` finds the
sysroot in `$PREFIX/sysroots/...` (§3.2), so neither `PYC_SYSROOT` nor the
checkout is needed at compile time (the host still needs clang++/LLVM 22 to
*link* user programs — §1.1). The download can be mirrored with
`PYC_GITHUB_REPO` / `PYC_SYSROOT_TAG` / `PYC_SYSROOT_ASSET`;
`--sysroot DIR` links an existing sysroot instead (`--copy-sysroot` copies,
rewriting the manifest paths); `--build-sysroot` compiles CPython instead
of downloading. Uninstall is marker-guarded (`lib/pyc/pyc-prefix.json`)
and never touches the prefix itself or anything pyc does not own — a
symlinked sysroot loses the link, not its target.

To keep working inside the checkout (no prefix):

```bash
# one script: checks host tools -> fetches (or builds) the sysroot ->
# builds pyc -> compiles+runs a smoke program, and prints your ready line
./tools/setup-machine.sh
```

`setup-machine.sh` installs the sysroot to
`$HOME/opt/py-sysroots/cp314-3.14.7-tier1` and builds the compiler in-tree;
it then tells you the one env var that makes everything work:

```bash
export PYC_SYSROOT=$HOME/opt/py-sysroots/cp314-3.14.7-tier1
./compiler/tools/pyc hello.py -o hello
./hello
```

Useful variants:

```bash
./tools/setup-machine.sh --beside        # sysroot next to the tree
                                                # (compiler/tools/sysroot);
                                                # no env var needed at all
./tools/setup-machine.sh --build-sysroot  # compile CPython instead of
                                                # downloading the tarball
./tools/setup-machine.sh --skip-compiler  # sysroot only
```

The manual equivalent, if you want to see each step:

```bash
make -C compiler CXX=clang++-22          # builds compiler/tools/pyc
pyc --fetch-sysroot or export PYC_SYSROOT=...   # one of the §3.1 paths
./compiler/tools/pyc hello.py -o hello
```

The building blocks `install.sh` composes, if you want the compiler
without a sysroot or a custom layout:

```bash
./tools/install-pyc.sh --prefix "$HOME/opt/pyc" --no-sysroot  # compiler only
export PATH="$HOME/opt/pyc/bin:$PATH"
pyc --fetch-sysroot          # into $HOME/opt/pyc/sysroots/...
pyc hello.py -o hello
```

### 1.4 Building a sysroot yourself

`build-python-sysroot.sh` is how the tarball is *produced* — use it when the
release asset is missing, you need a different CPython, or you are changing
the A2 runtime headers. It downloads and configures CPython, builds a
**Tier 1** sysroot (static `libpython.a`, dynamic libc, `-rdynamic`, PGO),
verifies it (interpreter runs with `LD_LIBRARY_PATH` stripped;
`math.factorial(25)` — CHARTER I2; manifest/interpreter version match), and
leaves it under `$PYC_SYSROOT_ROOT` (default `$HOME/opt/py-sysroots`),
named `<abi>-<version>-tier1`:

```bash
./tools/build-python-sysroot.sh                          # 3.14.7 tier 1
./tools/build-python-sysroot.sh --version 3.13.7         # a second target
./tools/build-python-sysroot.sh --prefix /data/sr        # elsewhere
./tools/build-python-sysroot.sh --no-wheel-check         # offline / faster
./tools/build-python-sysroot.sh --verify-only --prefix DIR
```

CPython's own build happens under `$PYC_SRC_ROOT` (default
`$HOME/build/cpython-sysroot`). A second CPython is a second directory there
(or under the prefix's `sysroots/`), not a second compiler — §6.

---

## 2. Compiling a program

```
pyc FILE [-o OUT] [--python=X.Y] [-std=pyX.Y] [--python-sysroot DIR]
        [-O0|-O1|-O2|-O3] [--trace] [--flamegraph]
        [--emit-llvm] [--keep] [--python-abi=ABI]
        [--fetch-sysroot] [--list-python-targets]
```

Contract (INTERFACES §5): exit `0` **iff** an executable was produced at
`-o`; any nonzero exit leaves no output file and prints diagnostics to
stderr (an unsupported construct names the construct, file, line, and
reason — CHARTER I1). Program output never goes to the compiler's stderr.

| Flag | Meaning |
|---|---|
| `-o OUT` | output binary (default `a.out`) |
| `-O0 … -O3` | optimisation for the generated code (default `-O1`; the runtime objects are always `-O2`). At `-O0` there is no LTO; otherwise thin LTO is enabled automatically when `ld.lld` is present. |
| `--python=X.Y` | select the sysroot by X.Y among the installed ones. Selects a *target*, not a second compiler (§6). A missing version is an error listing what is installed. |
| `-std=pyX.Y` | the language level the target parses at (feature version); it must be ≤ the runtime's. See `rebuild/VERSION_TARGETING.md`. |
| `--python-sysroot DIR` | use exactly this sysroot (must contain `pyc-sysroot.json` or `bin/python3*`). |
| `--emit-llvm` | write the generated LLVM IR (`-o hello.ll`) instead of a binary. |
| `--keep` | keep the scratch directory (generated IR, runtime objects) for inspection. |
| `--trace` / `--flamegraph` | link the instrumentation cores. Dormant unless activated at run time with `PYC_TRACE` / `PYC_FLAMEGRAPH` — §7. |
| `--fetch-sysroot` | download + install the sysroot (see §1.2), then continue or stop. |
| `--list-python-targets` | print the selected target and every sysroot pyc can see. |

What one compile does, end to end (one binary, no separate tools):

1. **Parse** — spawns the *sysroot's* interpreter,
   `<sysroot>/bin/python3.14 -m pyc_parse FILE` (with `LD_LIBRARY_PATH`
   stripped — §5.2), reads the typed AST as a JSON envelope. A parse error is
   rendered as `file:line:col`.
2. **Lower** — in-process: typed SSA (proven types, guarded unboxings).
3. **Codegen + link** — `PYC_CXX` (default `clang++`) compiles the runtime
   objects and links `mod.ll` + `main.cpp` + runtime objects +
   **`libpython.a` under `--whole-archive`**, with `-rdynamic` (Tier 1:
   `dlopen` keeps working, so C-extension wheels load).

The binary you get is a normal ELF executable: run it directly, ship it,
`ldd` it, profile it. It needs nothing from the compiler at run time — only
the standard library it was compiled against (§4).

---

## 3. The sysroot

### 3.1 What it is

A directory that is a complete, self-contained target CPython:

```
cp314-3.14.7-tier1/
  bin/python3.14        # the interpreter: pyc's parse oracle (I5) and the
                        # thing you run to check your program's CPython side
  include/python3.14/   # headers the link uses
  lib/.../libpython.a   # the static libpython your binary links (Tier 1)
  lib/python3.14/       # the standard library the binary finds at run time
  pyc-sysroot.json      # the manifest: version, abi, tier, machine, glibc,
                        # absolute paths (interpreter, headers, libpython)
```

`pyc-sysroot.json` is what makes a directory *a* sysroot; the install
scripts rewrite its absolute paths when the tree moves, so a fetched or
copied sysroot works wherever it lands.

### 3.2 How pyc finds one

In order; the first valid directory wins (valid = has `pyc-sysroot.json` or
`bin/python3*`):

1. `--python-sysroot DIR` (or `--sysroot DIR`)
2. `PYC_SYSROOT`
3. exe-relative, with `--python=X.Y` / `PYC_PYTHON` filtering by X.Y:
   - `<pyc dir>/sysroot`
   - `<pyc dir>/../sysroot`
   - `<pyc dir>/../sysroots/cp*-tier*/` (the installed-prefix layout)
   - every directory under `$HOME/opt/py-sysroots/`

If none is found, the error prints the directories it tried and the four
fixes:

```
pyc: no sysroot (need pyc-sysroot.json or bin/python3*)
  tried:
    /home/you/opt/pyc/sysroot
    ...
  pyc --fetch-sysroot
  # or: PYC_FETCH=1 pyc file.py
  # or: ./tools/setup-machine.sh --beside
  # or: --python-sysroot DIR / PYC_SYSROOT
```

A wrong directory is refused, not guessed at:
`pyc: PYC_SYSROOT is not a sysroot: ...  (need pyc-sysroot.json or bin/python3*)`.

A missing sysroot is a **compile error**. pyc never falls back to the
`python3` on your PATH — that would parse with one CPython and link against
another, which is the exact confusion the sysroot exists to remove (I5).

---

## 4. Running and deploying

The binary embeds the absolute path of the sysroot it was compiled against.
At startup it finds its standard library in this order:

1. `PYTHONHOME`, if set (must contain `lib/python3.14`);
2. `lib/python3.14` **next to the executable** (or its parent directory);
3. the compile-time sysroot.

If none exists, the binary exits with a diagnostic that names what it
searched and the fix — not CPython's bare `Failed to import encodings
module`.

A **portable deployment** is the binary plus the stdlib folder:

```
app/
  myprog                # compiled binary
  lib/python3.14/       # copied from the sysroot
```

```bash
cp -r "$SYSROOT/lib/python3.14" app/lib/
./app/myprog            # finds app/lib/python3.14 next to the binary
# or, without copying:
PYTHONHOME="$SYSROOT" ./myprog
```

The binary is Tier 1: `ldd` shows no `libpython` entry, yet
`import numpy` / `dlopen` of C-extension modules still works (§8).

---

## 5. Environment variables

### 5.1 Set, at compile time (driver)

| Variable | Meaning |
|---|---|
| `PYC_SYSROOT=DIR` | use this sysroot (2nd in §3.2). One variable serves the driver **and** the differential harness as its oracle — that coupling is deliberate (I5: subject and oracle must be the same CPython). |
| `PYC_PYTHON=X.Y` | same as `--python=X.Y` (select among installed sysroots). |
| `PYC_CXX=clang++-22` | the C++ compiler for the link stage (default `clang++`; it must be LLVM 22). |
| `PYC_FETCH=1` | fetch the sysroot on first compile when none is found (same effect as running `pyc --fetch-sysroot` once). |
| `PYC_GITHUB_REPO`, `PYC_SYSROOT_TAG`, `PYC_SYSROOT_ASSET` | override the fetch source (defaults: `joelandman/pyc`, `sysroot-cp314-linux-x86_64`, `cp314-3.14.7-tier1-x86_64.tar.xz`). |
| `PYC_SYSROOT_ROOT=DIR` | where `build-python-sysroot.sh` installs (default `$HOME/opt/py-sysroots`). |
| `PYC_SRC_ROOT=DIR` | where `build-python-sysroot.sh` builds CPython (default `$HOME/build/cpython-sysroot`). |

### 5.2 Set, at run time (the compiled binary)

| Variable | Meaning |
|---|---|
| `PYTHONHOME=DIR` | explicit stdlib location (1st in §4's order). |
| `PYC_TRACE=FILE` | activate tracing in a `--trace` build: Perfetto (Chrome-JSON) trace of the run (one NDJSON line per event). |
| `PYC_TRACE_LINES=1` | additionally emit LINE events (opt-in: per-event cost, ~16× on a function-dense program). |
| `PYC_FLAMEGRAPH=FILE.svg` | activate the sampler in a `--flamegraph` build: 99 Hz stack sampling, self-contained SVG at exit. |
| `PYC_FLAMEGRAPH_HZ=N` | sample rate (default 99). |

Without the `PYC_TRACE`/`PYC_FLAMEGRAPH` variables an instrumented build is
behaviourally identical to an unflagged build (a CI-gated invariant;
overheads in `rebuild/TRACING.md`).

### 5.3 Set, for the dev harness (`verify/`)

| Variable | Meaning |
|---|---|
| `PYC_BINARY=/path/to/pyc` | which pyc the harness compiles with (else: `compiler/tools/pyc` from any cwd). |
| `PYC_SYSROOT` | the harness oracle (same variable as the driver's — both sides are the same CPython by construction). |

### 5.4 Unset — the one that bites

**`LD_LIBRARY_PATH`.** The sysroot's `bin/python3.14` is a thin launcher over
its own `libpython3.14.so.1.0` (RUNPATH). A caller's `LD_LIBRARY_PATH`
(conda, `/home/.../local`, a distro python) **outranks RUNPATH** and silently
swaps in a different libpython — then the "oracle" is not the CPython your
program embeds, and nothing downstream notices, because the version string
usually matches.

- pyc handles this for you at compile time: the parse subprocess runs with
  `LD_LIBRARY_PATH` stripped.
- When **you** run the sysroot's interpreter (as an oracle, for a
  differential check, `pip install`, …), strip it yourself:

  ```bash
  env -u LD_LIBRARY_PATH "$SYSROOT/bin/python3.14" prog.py     # CPython side
  ./prog                                                       # pyc side (unaffected: libpython is static)
  ```

  A compiled binary itself is never affected — it links `libpython.a`
  statically and has no `DT_NEEDED` for it.

- The harness pins this for both sides (`child_env` strips
  `LD_LIBRARY_PATH` and also fixes `PYTHONHASHSEED`, `PYTHONDONTWRITEBYTECODE`,
  and the colour variables, so a difference is a difference in the programs,
  not the terminal).

---

## 6. Targeting more than one CPython

A target is a sysroot directory, not a compiler flag. Install a second
sysroot (a tarball or §1.4) and select it:

```bash
./tools/build-python-sysroot.sh --version 3.13.7        # cp313-3.13.7-tier1
pyc --python=3.14 app.py -o app-314
pyc --python=3.13 app.py -o app-313
pyc --list-python-targets                               # what pyc can see
# selected 3.14  abi=cp314
#   sysroot      /home/you/opt/py-sysroots/cp314-3.14.7-tier1
#   interpreter  /home/you/opt/py-sysroots/cp314-3.14.7-tier1/bin/python3.14
# installed:
#   3.14  abi=cp314   /home/you/opt/py-sysroots/cp314-3.14.7-tier1
#   3.13  abi=cp313   /home/you/opt/py-sysroots/cp313-3.13.7-tier1
```

`-std=pyX.Y` pins the language level below the runtime (`rebuild/VERSION_TARGETING.md`);
`--python-abi=...` selects among same-version ABIs (e.g. a freethreaded
`cp314t` sysroot). A requested version that is not installed is an error
listing what is installed — never a silent fallback.

---

## 7. Observing a run: trace and flamegraph

Build once with the flag(s); activate per run with the env var — the same
binary serves clean and measured runs:

```bash
pyc hot.py -o hot --trace --flamegraph

./hot                                   # clean run: no env vars, no overhead
PYC_TRACE=hot.ndjson ./hot              # Perfetto trace (Chrome-JSON NDJSON)
PYC_FLAMEGRAPH=hot.svg ./hot            # 99 Hz sampling -> self-contained SVG
PYC_TRACE=hot.ll.json PYC_TRACE_LINES=1 ./hot   # with line events (opt-in)
```

Reading them, measured (TRACING.md §6): event traces are the right tool for
call-dense code (ast-parse of a 1.3 MB source: 6 MB trace); on
generator-dense code every generator step is an event and the trace becomes
infeasible (a 1 s matmul workload wrote 3.6 GB) — there, and on
arithmetic-dense hot loops (native loops fire almost no events), the
**flamegraph is the observation**. The `verify/perf_analyze.py` tool reads
both artifacts side by side.

---

## 8. C-extension wheels (NumPy & friends)

Tier 1 links `-rdynamic`, so `dlopen` works: a compiled binary loads
prebuilt wheels exactly like CPython does. The published sysroot ships
without third-party packages, so install into a **copy** of the sysroot
(the primary stays pristine; the binary finds packages through the sysroot
it was linked against):

```bash
SRC=$HOME/opt/py-sysroots/cp314-3.14.7-tier1
DST=$SRC-numpy
cp -a "$SRC" "$DST"
sed -i "s|$SRC|$DST|g" "$DST/pyc-sysroot.json"      # manifest paths -> the copy
env -u LD_LIBRARY_PATH "$DST/bin/python3.14" -m pip install numpy

PYC_SYSROOT="$DST" pyc wheel.py -o wheel            # wheel.py: import numpy; ...
./wheel
ldd wheel | grep -i libpython                       # -> (nothing)
```

Verified end to end in CI on every push (numpy import, output equal to the
sysroot CPython, no libpython `DT_NEEDED`).

---

## 9. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `pyc: no sysroot (need pyc-sysroot.json or bin/python3*)` + list of dirs tried | no sysroot in the §3.2 search | `pyc --fetch-sysroot`, or `export PYC_SYSROOT=...`, or `./tools/setup-machine.sh --beside`, or `--python-sysroot DIR` |
| `pyc: PYC_SYSROOT is not a sysroot: DIR` | DIR has neither `pyc-sysroot.json` nor `bin/python3*` | point it at the sysroot **root** (the dir containing those), not a parent |
| `pyc: no sysroot for --python=3.13` + "installed:" list | that version is not installed | install it (§1.4 / tarball), or use a version that is listed |
| binary exits: CPython `Failed to import encodings module` / pyc's stdlib diagnostic | the sysroot's `lib/python3.14` moved or was deleted, and no §4 source exists | copy `lib/python3.14` next to the binary, or set `PYTHONHOME`, or reinstall the sysroot |
| `pyc: unknown option '-g'` | only the §2 flags exist; the §5 contract lists future flags the driver does not yet accept | drop the flag |
| CPython-side check "fails" with a version-matching interpreter | a shadowed `libpython` on `LD_LIBRARY_PATH` (conda, local) outranks the sysroot's RUNPATH | `env -u LD_LIBRARY_PATH "$SYSROOT/bin/python3.14" ...` (§5.4) |
| compile works here, binary won't start on the target box | target glibc older than the sysroot's build host (manifest `"glibc"`) | build/run the sysroot on a newer base, or deploy where the glibc matches |
| `pip install` inside the primary sysroot | the primary must stay pristine (it is the published/oracle sysroot) | install into a copy (§8) |
| `install.sh --uninstall` refuses: `not a pyc install: no .../pyc-prefix.json` | the prefix was not made by `install.sh` / `install-pyc.sh` | point `--prefix` at the right directory, or `--force` |
| `install.sh` (or `pyc --fetch-sysroot`) refuses the downloaded sysroot: `version mismatch: manifest claims X, interpreter is Y` | the published release asset was stamped against a different CPython build than its manifest claims | do not work around the check — republish the asset from a sysroot whose interpreter matches its manifest |

---

## 10. Where everything lives

```
# installed prefix (tarball, install.sh, or install-pyc.sh)
$PREFIX/
  bin/pyc                        # the compiler
  lib/pyc/{pyc_parse,include,src/rt,install-sysroot.sh,pyc-prefix.json}
  sysroots/cp314-3.14.7-tier1/   # --fetch-sysroot installs here

# a sysroot (anywhere; $HOME/opt/py-sysroots/ is the default search dir)
cp314-3.14.7-tier1/
  bin/ include/ lib/ share/ pyc-sysroot.json
```

`pyc-sysroot.json` fields: `version`, `xy`, `abi`, `abiflags`, `tier`,
`free_threaded`, `debug`, `machine`, `glibc`, `wheels_supported`,
`interpreter`, `include`, `libpython_static`, `libpython_shared`,
`sysroot`, `link_flags`, `builtin_modules`, `dynload_modules`,
`archive_name`.

Related docs: `README.md` (status and why), `rebuild/CHARTER.md` (the
invariants these procedures protect), `rebuild/VERSION_TARGETING.md`
(`--python` / `-std` semantics), `rebuild/TRACING.md` (trace/flamegraph
mechanism and overhead), `verify/README.md` (harness semantics).
