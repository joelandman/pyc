# pyc — Architecture Review

Date: 2026-08-22. All findings below were measured against the built
`build/pyc` binary as of commit `1098ade`, not inferred from documentation.

## Verdict

The hypothesis — "this was meant to be a general Python compiler and became a
toy for a curated subset" — is **correct in its conclusion and wrong about the
cause**.

There was no architectural blunder. There was no design document that chose a
subset. What was missing is subtler and more damaging: **there was never a
semantic specification, and the test suite could not detect its absence.**

## Correcting the previous review

The prior review's headline finding was that pyc has "a hand-written
recursive-descent parser for a curated subset — and no grammar file". That is
false, and it sent the analysis in the wrong direction.

`src/frontend/PythonParser.cpp` includes `<Python.h>` and calls `ast.parse`
through the CPython C API. **The project has complete Python grammar coverage
and always has.** That was its best decision. The 1,319 lines are not a parser
— they are an AST *converter*, and that is where the damage begins.

## Finding 1 — The AST is stringly-typed and lossy

`include/pyc/PythonParser.h` defines the sole AST node:

```cpp
struct ASTNode {
    std::string type, value, op, id;
    std::vector<std::string> args;
    std::vector<std::unique_ptr<ASTNode>> children;
};
```

`FunctionDef` conversion pushes body statements, `Default` wrappers, and
`Decorator` wrappers into the *same* `children` vector, to be separated
downstream by string comparison. `posonlyargs`, `kwonlyargs`, and annotations
are never read. Unknown node types reach a generic fallback that descends into
`.body` or `.value` and discards everything else.

Measured consequences:

| Source | CPython | pyc |
|---|---|---|
| `x: int = 5; print(x)` | `5` | `None` — silent |
| `def f(a,*,b=2,c=3): ...; f(1,b=10)` | `14` | runtime `TypeError` |
| `def g(a, /, b): ...` | `6` | `Module verification failed` |

## Finding 2 — The object model is a closed union

`include/pyc/object_struct.h` defines one flat struct with every payload
inlined: two `vector<PyObject*>`, a `vector<pair<...>>`, a `std::string`, a
`vector<int64_t>`, a `vector<double>`. Compiled and measured: **208 bytes for
every object, including the integer `5`.**

The type is `int type` — a closed tag, values 0–20. There is no type object,
no method slots, and no MRO in the value representation. Two consequences
follow necessarily: user-defined classes cannot be first-class, and every
operation must switch on the tag.

## Finding 3 — Semantics are approximations, so failures are silent

| Source | CPython | pyc |
|---|---|---|
| `math.factorial(25)` | `15511210043330985984000000` | `7034535277573963776` |
| `len("héllo wörld")` | `11` | `13` |
| `"héllo".upper()` | `HÉLLO` | `HéLLO` |
| `sum(It())` with `__iter__`/`__next__` | `6` | `TypeError: not iterable` |
| `next(gen())` | `0` | `None` |

`int` is a machine word that wraps without error. `str` is UTF-8 bytes rather
than code points. Both were presumably chosen for speed; both produce wrong
answers with no diagnostic — the worst failure class a compiler has.

## Finding 4 — Features are implemented at callsites, not as protocols

Note rows 4 and 5 above: `__iter__` is honoured inside a comprehension but not
by `sum()`; generators work through `list()` but `next()` returns `None`.
These are not bugs in a design — they *are* the design. `Compiler.cpp` carries
815 string comparisons, 99 of them in a single `methodName == "..."` if-else
chain. The project maintains `tests/check_dispatch_chain.py`, a static guard
whose purpose is to detect **unreachable arms** in that chain, added after
`Counter.update` shipped a runtime function that never executed.

A dispatcher that requires a static analysis to find its own dead code is the
architecture reporting its condition.

## Finding 5 — The test suite is the drift mechanism

`tests/runner.py` holds 662 inline `(source_string, expected_output_string)`
pairs, with the expected string documented as the source of truth. Such a
suite measures "does the curated subset still work". It can remain green
indefinitely while the gap to real Python never narrows.

This is how the general-purpose intent was lost. Every unit of work arrived as
"make this snippet pass". The cheapest way to satisfy that is a special case
at the callsite. Six hundred and sixty-two green cases feel exactly like
progress, and each one made the next general fix more expensive. No individual
decision was wrong; the gradient pointed downhill the entire way.

## What is genuinely good

The deployment story — the actual product — **already works**:

```
$ pyc h.py -o hs.exe --static
$ file hs.exe
ELF 64-bit LSB executable, x86-64, statically linked, stripped   # 2.9 MB
$ ldd hs.exe
        not a dynamic executable
```

The dynamic build carries only four non-libc dependencies. Multi-module import
of user `.py` files works. Also sound: compiling the runtime to LLVM bitcode
and LTO-ing it into user programs (the right way to make a boxed runtime
disappear), the `-g`/DWARF work, the opt-level structure, and the empirical
unboxing knowledge — native int/float locals, homogeneous list layouts, native
`range` loops. The last is real, hard-won, and transfers directly, even though
it must be rebuilt as an analysis rather than as fast paths.

## Strategic position

The occupied quadrants are worth naming, because pyc is currently standing in
the worst one:

- **Codon** (MIT, LLVM, AOT) chose semantic divergence for speed — its `int`
  is an int64, exactly pyc's choice. It is well-funded and remains a subset
  that cannot run most Python. **pyc has been on the Codon path with a much
  smaller team.**
- **Nuitka** compiles to C and links libpython: runs essentially all Python
  including wheels, with modest speedups.
- **mypyc** compiles a typed subset to C and ships real software.
- **PyPy** is a JIT, not AOT, and C extensions remain painful.

The stated goals — most Python code, prebuilt binaries, `--static`, PyTorch
wheels — sit squarely in Nuitka's quadrant. The differentiators available are
LLVM-quality typed optimization and genuinely self-contained static binaries,
neither of which that quadrant has nailed.

## Decisions taken

1. **Adopt CPython's object model and link libpython.** Bignum `int`, correct
   Unicode, real stdlib, and C-extension wheels all arrive at once, and the
   compiler stops reimplementing a language it was only approximating.
2. **New tree, port the learnings.** The 13.4k-line string-dispatch lowering
   and the 208-byte `PyObject` are what cap the ceiling; carrying them forward
   carries the ceiling forward.
3. **Differential CI plus CPython `Lib/test/` as the published metric.** The
   constraint whose absence caused the drift.

See `CHARTER.md` for the binding invariants and `AGENT_DIRECTIVES.md` for the
instruction set that operationalises them.
