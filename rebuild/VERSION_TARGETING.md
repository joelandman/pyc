# Python Version Targeting

**Status:** design, settled. Binding on A0–A5.

## The ask

Select the targeted Python major.minor the way a C compiler selects a language
standard: `pyc prog.py --python=3.14`.

## Correcting the analogy first

`-std=c11` in gcc/clang selects a **language dialect only**. The C library is
chosen independently — that separation is why `-std=` is one small flag.

Python does not separate that way. A "version" bundles four things that ship
together and cannot be mixed freely:

| Axis | What varies | Separable? |
|---|---|---|
| **Syntax** | accepted grammar (`match` 3.10, `type X` 3.12, t-strings 3.14) | **yes** |
| **AST schema** | node kinds and field types | no — follows syntax |
| **C-API / ABI** | `libpython`, struct layouts, wheel tags | no — follows runtime |
| **stdlib** | `Lib/`, frozen modules | no — follows runtime |

So the honest analogy is not `-std=`. It is clang's **`--target=` plus
`--sysroot=`**: a target triple naming the runtime, and a sysroot providing
that target's headers, libraries, and platform code. `-std=` maps onto the one
axis that genuinely *is* separable — syntax.

That gives two flags, not one, and the split earns its keep (see below).

## Measured facts this design rests on

All measured on this machine, 2026-08-22, CPython 3.12.3 and 3.14.7:

1. **The AST delta between minor versions is tiny.** 3.12 → 3.14 adds exactly
   two node kinds (`TemplateStr`, `Interpolation` — PEP 750) and drops one
   deprecated alias. Out of 133 node kinds total. Cumulative history is
   similarly small: walrus (3.8, 1 node), `match` (3.10, 9 nodes), type
   parameters (3.12, 4 nodes), t-strings (3.14, 2 nodes).
   **Your intuition is correct: nearly all of the compiler is version-invariant.**

2. **`ast.parse(feature_version=(3,Y))` gates syntax exactly as `-std=` does —
   but only downward.** Verified: a 3.14 parser rejects `type X = int` at
   `feature_version=(3,11)` and accepts it at `(3,13)`; rejects t-strings below
   `(3,14)`. It can never parse syntax *newer* than the interpreter running it.

3. **`sizeof(PyObject)` is 16 bytes on both 3.12 and 3.14** — stable across
   these versions. (For contrast, the old tree's flat struct was 208 bytes.)

4. **Wheel tags are version-specific**: `cp312-cp312-*` vs `cp314-cp314-*`.
   `abi3` variants exist and are accepted by both, but the major numeric wheels
   (NumPy, PyTorch) ship version-specific builds. ABI is therefore a hard
   per-target constraint, not a preference.

5. **`_field_types` exists from 3.13 onward** and carries the complete typed
   schema reflectively — field types, optionality (`ast.expr | None`), and
   sequences (`list[ast.stmt]`). Everything `Python.asdl` encodes:

   ```
   FunctionDef._field_types
      name            <class 'str'>
      args            <class 'ast.arguments'>
      body            list[ast.stmt]
      decorator_list  list[ast.expr]
      returns         ast.expr | None
      type_params     list[ast.type_param]
   ```

   3.12 does **not** have it. And no `.asdl` file ships in an installed
   CPython — it lives only in the source tarball.

## Design

### The Python Target Description (PTD)

A target is **data, not code**. One `pyc` binary; targets are resolved records:

```
PTD {
  version        3.14                     # major.minor
  abi            cp314 | cp314t | abi3    # t = free-threaded; separate ABI
  sysroot        /path/to/target/root     # headers, libpython, stdlib
  interpreter    <path>                   # the oracle: parse + differential test
  libpython      libpython3.14.a | .so
  asdl_schema    <generated>              # from _field_types (>=3.13) or ASDL
  wheel_tags     [cp314-cp314-manylinux…, cp314-abi3-…]
}
```

Nothing in the compiler hardcodes a version. Adding 3.15 support means adding
a sysroot and regenerating one file — not editing the compiler.

### Flag surface

```
--python=3.14              Target runtime: ABI + stdlib + syntax. Default: host.
-std=py3.11                Syntax level ONLY. Default: matches --python.
--python-abi=cp314t        ABI variant (free-threaded, abi3). Default: cp<ver>.
--python-sysroot=PATH      Override target root. Default: from registry.
--list-python-targets      Show installed, usable targets and why any are not.
```

**Why `-std=` stays separate from `--python`.** It is the one axis that is
genuinely independent, and separating it buys a real capability: *run on 3.14,
but refuse syntax that would not compile on 3.11.* That is a portability lint
you cannot express with a single flag, and it is exactly what `-std=` means to
a C programmer. `-std=py3.15` with `--python=3.14` is an error — you cannot
target a runtime older than the syntax you accept.

### Where parsing happens — the one real decision

Two options, and the choice matters more than it looks:

**(a) Link host libpython into `pyc`; parse in-process; downgrade with
`feature_version`.** Fast, but the compiler binary is welded to one CPython,
and by fact 2 it can never target a version *newer* than the one it links.
This is what the old tree did.

**(b) Invoke the *target* interpreter to parse, and serialize the AST back.**
The AST is then exactly the target's, by construction. No host≥target
constraint. `pyc` itself links no libpython at all, so the compiler binary is
version-agnostic and trivially distributable.

**Adopt (b).** The subprocess and serialization cost is negligible beside LLVM
codegen, and it buys three things (b) alone provides: targeting a CPython newer
than the one pyc was built against, a compiler binary with no libpython
dependency, and an AST guaranteed to match the target rather than approximated
from the host. Keep (a) as a pure optimization for the `host == target` case,
behind a flag, only once profiling justifies it.

Note this also makes the parse oracle and the *differential-test* oracle
(CHARTER I5) the same binary — which is what you want, since a divergence must
be measured against the runtime actually being targeted.

### AST generation across versions

Generate the typed AST (CHARTER I4) from the target, in this order:

1. **Target ≥ 3.13:** introspect `_field_types` from the target interpreter.
   Complete typed schema, no source tarball needed. Preferred.
2. **Target ≤ 3.12:** parse `Parser/Python.asdl` from that version's source
   tarball. Required, because `_field_types` does not exist there.

Generated output is a **version-tagged module** (`ast_3_14.hpp`). The shared,
version-invariant core is written once against the union of node kinds; each
version's generated module marks which kinds exist. A node kind absent from the
target is a **compile-time diagnostic naming the construct, the version that
introduced it, and the target** — never a silent fallthrough (I1):

```
error: t-string literals require Python 3.14 or later
  --python=3.12 selected (from PYC_PYTHON)
  prog.py:14:9
```

### What this costs when 3.15 lands

1. Add a 3.15 sysroot; run the generator against its interpreter.
2. The build **fails** on every non-exhaustive match over the AST — that is
   I4 doing its job, and it is the entire list of work to be done.
3. Fix those arms. Empirically (fact 1) that is a handful of node kinds.
4. Run the differential suite against the 3.15 oracle.

No compiler surgery. That is the property you asked for, and it is achievable
because the version-variant surface really is this small.

## Consequences for the CPython rebuild

Before rebuilding 3.14.7 with a static `libpython`, note that the sysroot is
**per (version, ABI)** — so decide now which of these you need, since each is a
separate CPython build:

**Correction (2026-08-22):** an earlier draft claimed the existing 3.14.7 build
lacked `libpython3.14.a`. It does not. The file is in `LIBPL`
(`lib/python3.14/config-3.14-x86_64-linux-gnu/`), CPython's canonical location,
not `lib/`. `--with-static-libpython` defaults to yes and `--enable-shared`
does not suppress it. **The existing build already serves both link modes for
Tier 1**, and no rebuild is needed for it.

A sysroot is keyed by **(version, ABI, link tier)**:

- **`cp314` stock** — the existing `/home/joe/local` build. Serves the dynamic
  binary *and* Tier-1 static-libpython (`-rdynamic`), and loads `cp314` wheels.
  Ready now.
- **`cp314` fully-static** — a *separate* build required for Tier 2, configured
  `--disable-shared` with stdlib extensions moved into `Modules/Setup.local` as
  builtins. Without it, a `-static` binary cannot `import math` (77 of the
  stdlib's C modules are `dlopen`-ed `.so` in a stock build). This sysroot can
  never load C-extension wheels — see CHARTER I7.
- **`cp314t` free-threaded** — a distinct ABI with distinct wheel tags,
  requiring its own build. Not a flag that can be added later.
