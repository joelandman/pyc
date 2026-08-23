# compiler/ — the rebuilt pyc

New tree. Contracts in `../rebuild/INTERFACES.md`, invariants in
`../rebuild/CHARTER.md`. The legacy tree under `../src/` is not carried
forward; see `../rebuild/ARCHITECTURE_REVIEW.md` for why.

## A1 — frontend (in progress)

```
pyc_parse/            run BY THE TARGET INTERPRETER (INTERFACES §2.1)
  encode.py           ast -> JSON, total by construction
  decode.py           JSON -> ast; reference semantics for the C++ reader
  __main__.py         python3.X -m pyc_parse FILE [--feature-version 3.Y]
tools/
  extract_schema.py   target interpreter -> normalized schema JSON
  gen_ast.py          schema -> include/pyc/ast/generated.hpp
  roundtrip.py        the totality proof (see below)
  lint_visitors.py    enforces the generic-arm ban
include/pyc/ast/
  support.hpp         hand-written: SourceLoc, Box<T>, ConstantValue, ov
  generated.hpp       GENERATED — do not edit, regenerate
```

### Regenerating

```bash
SYSROOT=~/opt/py-sysroots/cp314-3.14.7-tier1
$SYSROOT/bin/python3.14 compiler/tools/extract_schema.py -o /tmp/schema.json
./compiler/tools/gen_ast.py /tmp/schema.json -o compiler/include/pyc/ast/generated.hpp
```

A new CPython node kind then breaks every non-exhaustive `std::visit`. That is
I4 working; fix the arms, never add a generic fallback.

### Why the encoder is reflective

`encode.py` walks `_fields`/`_attributes` and special-cases nothing per node
type. The old converter hand-listed fields per node and dropped the rest —
which is why `x: int = 5` evaluated to `None`, `def g(a, /, b)` crashed
codegen, and `posonlyargs`/`kwonlyargs`/annotations were never read at all.

Two places JSON quietly loses Python, both handled:

- **`int` is arbitrary precision.** Encoded as a decimal *string*; a JSON
  number silently truncates above 2^53. `ConstBigInt` keeps it as text in C++
  too — storing it in an `int64_t` is exactly how the old runtime wrapped
  `math.factorial(25)`.
- **`float` via `repr`**, so `inf`/`nan` and exact round-trips survive.
  `bytes` are base64; `complex`/`tuple`/`frozenset`/`Ellipsis`/`None` are each
  tagged, and `bool` is tested before `int` because it is a subclass.

### The totality proof

```bash
./compiler/tools/roundtrip.py --stdlib
```

parse → encode → real JSON → decode → compare `ast.dump(include_attributes=True)`.
Comparing *with* attributes is stricter than required: it proves source
locations survive, which `SourceLoc` and `-g` depend on. Files that do not
parse under the interpreter are skipped and excluded from the denominator —
`Lib/test` ships deliberate bad-syntax fixtures.

### Deliberate over-inclusion

The schema keeps `AugLoad`, `AugStore` and `Param`, which are vestigial in
modern Python but remain real classes. Over-inclusion costs a dead visitor arm;
under-inclusion would be a silent miss. I1 makes that trade obvious.
