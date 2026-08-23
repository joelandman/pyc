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

## Totality result (2026-08-22)

`INTERFACES.md` §2.4 requires the round-trip to hold on **two** targets.

| Target | Files | ok | mismatch | error | skipped |
|---|---|---|---|---|---|
| CPython 3.14.7 | 14,641 | **14,637** | 0 | 0 | 4 |
| CPython 3.13.15 | 2,582 | **2,578** | 0 | 0 | 4 |

Both **TOTAL**: every node, field, and source attribute survived
parse → encode → real JSON → decode → `ast.dump(include_attributes=True)`.

The corpora differ in size because the 3.14 install carries a full
scientific stack (numpy, sympy, torch, nuitka) while the fresh 3.13 sysroot has
only numpy. Both cover the whole stdlib and `Lib/test`. The 4 skips on each are
files that do not parse under that interpreter — deliberate bad-syntax
fixtures, reported rather than hidden.

Schema delta between the targets is exactly the expected two nodes
(`TemplateStr`, `Interpolation` — PEP 750), with no field drift. The 3.13
header contains no `TemplateStr`; both compile independently from one
toolchain. That is I8: targets are data.

### What the two targets caught

Only running the second one exposed that `Constant` was being **silently
dropped** from the 3.13 schema: CPython keeps `Num`/`Str`/`Bytes`/
`NameConstant`/`Ellipsis` as subclasses of `Constant` through 3.13 and removes
them in 3.14, so "has subclasses" read as "is abstract" and a 3.13 header would
have had no literals in the language at all. 3.14 alone was clean. One target
proves the schema; two prove I8.

`_ALIASES` in `extract_schema.py` is a **curated list with no programmatic
marker** — review it whenever the target version moves, because this failure
mode is silent by construction.

## C++ deserializer fidelity (2026-08-22)

| Target | Files | ok | mismatch | error | skipped |
|---|---|---|---|---|---|
| CPython 3.14.7 | 14,641 | **14,637** | 0 | 0 | 4 |
| CPython 3.13.15 | 2,582 | **2,578** | 0 | 0 | 4 |

Checked structurally: the C++ reader emits a node-kind histogram and CPython
counts the same tree, so any dropped subtree, field, or node kind changes a
count. **FAITHFUL on both targets** — 17,215 files, one toolchain, target
supplied as data.

### The three under-described corners of CPython's reflection

`_field_types` is the schema source, and it under-describes itself in three
places. Each was a silent guess that real code turned into a real bug, and each
is now a curated list whose *absence* fails loudly:

| Constant | Problem | Failure without it |
|---|---|---|
| `_ALIASES` | deprecated names are still subclasses (`Num` under `Constant` ≤3.13) | `Constant` dropped entirely from the 3.13 schema |
| `_NULLABLE_ELEMENTS` | `None` *inside* a list, not expressed by `list[ast.expr]` | `{**a, **b}` and kwonly-without-default shift every later element |
| `_OBJECT_FIELDS` | `object` means three different things | t-strings and `case None:` fail to deserialize |

All three must be reviewed when the target version moves. `_NULLABLE_ELEMENTS`
is empirical (a scan of all 14,641 files found exactly two fields);
`_OBJECT_FIELDS` is enforced — an unlisted `object` field makes `gen_ast.py`
refuse to generate rather than guess.

## A2 — C-API binding (in progress)

```
include/pyc/rt/capi.hpp         CApiSymbol (INTERFACES §4)
include/pyc/rt/capi_table.hpp   GENERATED — 813 symbols
src/capi.cpp                    lookup()
tools/gen_capi_table.py         refcounts.dat -> the table
```

Derived from CPython's own `Doc/data/refcounts.dat`, not hand-written.
Maintaining refcount contracts for 800+ functions by hand is the bookkeeping
that produces leaks, and CPython already ships the answer.

| | count |
|---|---|
| symbols | 813 |
| returns a new reference (`Owned`) | 312 |
| returns a borrowed reference | 52 |
| always returns NULL | 16 |
| steal-annotated | 6 |
| banned | 1 |

### What that file is and is not authoritative for

**Returns: authoritative.** `+1` Owned, `0` Borrowed, `null` always-NULL,
blank means not a `PyObject*`.

**Parameter stealing: absent.** Its own header says so, and it is verifiable —
`PyList_SetItem`'s stolen `item` records `0`, indistinguishable from a borrow.
Stealing is therefore a curated list in the generator, validated against the
data: a name that does not match makes the generator **refuse to run**. That
guard is not decorative — three of the first six entries silently failed to
match, because the doc's parameter names are not uniform (`PyList_SetItem`
calls it `item`; `PyTuple_SetItem` calls the same argument `o`). An unapplied
steal annotation makes lowering emit a `DECREF` on a stolen reference: a
double free.

`may_raise` is deliberately conservative — everything except `void` is assumed
fallible. A redundant error edge is something A3 can optimise away; a missing
one loses an exception, which is a silent wrong answer (I1).

`_BANNED` carries a reason and a replacement. `PyModule_AddObject` steals
*only on success*, so its cleanup path differs by outcome and cannot be one
static contract; `PyModule_AddObjectRef` never steals. A contract that cannot
be expressed statically is one we refuse rather than approximate.

**Known gap:** `refcounts.dat` is not exhaustive of the C-API —
`PyModule_AddObjectRef` is absent from it, so the recommended replacement is
not yet in the table either. Symbols outside the table cannot be emitted by
lowering (INTERFACES §4), so this needs a supplementary source before A3 can
use them.
