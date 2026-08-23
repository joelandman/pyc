# Layer Interfaces

**Status: frozen v1.1 (2026-08-22).** Owned by A0. Changes require A0 sign-off, a
version bump, and a note in this file's changelog. A1–A4 build against these;
they are what let the layers proceed independently.

Language: **C++20**. AST transport across the parse boundary: **JSON**.

---

## 0. Layer map

```
  target CPython (the oracle, VERSION_TARGETING.md)
        │  ast.parse, feature_version = -std
        ▼  JSON envelope over stdout
  ┌───────────────┐
  │ A1  frontend  │  generated typed AST
  └───────┬───────┘
          │  §2  pyc::ast
  ┌───────▼───────┐
  │ A3  lowering  │  type lattice + dataflow
  └───────┬───────┘
          │  §3  pyc::ir  (typed SSA)
  ┌───────▼───────┐        ┌──────────────────┐
  │ A4  backend   │◄──§4───┤ A2 runtime/C-API │
  └───────┬───────┘        └──────────────────┘
          │  §5  driver CLI
          ▼
     native binary            A5 consumes §5 only
```

A5 depends on **§5 alone**. That is deliberate: the harness must survive every
internal change, so it may never reach past the CLI.

---

## 1. Cross-cutting

### 1.1 Diagnostics (I1)

Every layer reports through one type. A layer that cannot proceed emits an
`Error` and stops. Silent fallthrough is banned.

```cpp
namespace pyc {
struct SourceLoc { std::string file; int line = 0; int col = 0; };

struct Diagnostic {
    enum class Severity { Error, Warning, Note };
    Severity    severity;
    std::string message;      // human-readable, imperative
    SourceLoc   loc;
    std::string construct;    // AST node kind or feature, e.g. "TemplateStr"
    std::optional<std::string> introduced_in;  // e.g. "3.14", when version-gated
};

class DiagnosticSink {
public:
    virtual ~DiagnosticSink() = default;
    virtual void report(Diagnostic) = 0;
    virtual bool had_error() const = 0;
};
}
```

**Rule.** An unsupported construct produces an `Error` naming `construct` and
`loc`. Never a wrong value, never a silent skip. This is the mechanism by which
coverage gaps present as P2 `COMPILE_ERROR` in the harness rather than as P0
silent wrong answers.

### 1.2 Tree depth (binding on §2 and §3)

**Any traversal of `pyc::ast` or `pyc::ir` must survive a tree ~600 levels
deep.** This is a property of real code, not a pathology: sympy's
`polys/numberfields/resolvent_lookup.py` has an AST **568 levels deep**, and
CPython's own parser handles it. A recursive walker with a fat frame per level
will exhaust a default 8 MB stack well before that.

Measured 2026-08-22: a naive recursive encoder blew Python's 1000-frame limit
on that file, because each AST level costs several frames. The same arithmetic
applies to a C++ recursive descent.

Consequences, binding:

- A recursive traversal MUST be depth-bounded and fail with a §1.1
  `Error` naming the limit — never a stack overflow, which is an
  uncontrollable crash and cannot be reported as a diagnostic (I1).
- Prefer an explicit worklist/stack over native recursion in the hot walkers
  (A1's deserializer, A3's lowering). Native recursion is acceptable only
  where the depth is provably bounded by something other than user input.
- A thread that walks user trees must be given a stack sized for this
  explicitly, not left at the platform default.

A stack overflow here would present as a P1 `CRASH` in the harness at best,
and as a silent miscompile at worst — both avoidable by treating depth as an
input parameter rather than an assumption.

### 1.3 Target description

Defined in `VERSION_TARGETING.md`; consumed by A1 (schema + parse), A2
(headers, libpython) and A4 (link, wheel tags). Loaded from the sysroot's
`pyc-sysroot.json`.

---

## 2. §2 — Frontend → Lowering (A1 → A3)

### 2.1 Parse boundary

pyc invokes the **target** interpreter and reads a JSON envelope from stdout.
pyc itself links no libpython.

```
<sysroot>/bin/python3.X -m pyc_parse <file> --feature-version 3.Y
```

```jsonc
{
  "schema_version": 1,          // envelope format; mismatch is a hard error
  "python_version": "3.14.7",   // must equal the target
  "feature_version": [3, 11],   // what -std selected
  "ast": { "_kind": "Module", ... }
}
```

A `schema_version` or `python_version` mismatch is an `Error`, not a warning:
silently accepting a tree from the wrong interpreter reintroduces exactly the
host/target confusion the design exists to remove.

JSON is an implementation detail behind this interface and may be swapped for a
binary format on profiling evidence, without touching §2.2.

### 2.2 Typed AST

`namespace pyc::ast`. **Generated**, never hand-maintained — from the target's
`_field_types` (3.13+) or `Python.asdl` (≤3.12). One struct per ASDL
constructor; every field present; `SourceLoc` on every node.

```cpp
namespace pyc::ast {

struct FunctionDef {
    std::string                name;
    Arguments                  args;         // posonly/kwonly/defaults, all of it
    std::vector<Stmt>          body;
    std::vector<Expr>          decorator_list;
    std::optional<Expr>        returns;
    std::vector<TypeParam>     type_params;  // 3.12+
    SourceLoc                  loc;
};

using Stmt = std::variant<FunctionDef, ClassDef, Return, Assign, AnnAssign,
                          /* ... every stmt constructor ... */>;
using Expr = std::variant<BinOp, Call, Name, Constant, TemplateStr /*3.14*/,
                          /* ... */>;
}
```

### 2.3 Encoding hazards (found by the §2.4 proof, not by review)

JSON looks able to carry a Python value and quietly cannot, in two places.
Both are settled; a future transport change must preserve both properties.

- **`int` is arbitrary precision.** Carried as decimal *text*. A JSON number
  truncates silently above 2^53 — the same failure as the old runtime's
  `math.factorial(25)`, relocated to the serializer. `ConstBigInt` keeps it as
  text in C++ too; materialising it is the runtime's job.
- **`str` is a sequence of code points and may hold lone surrogates.** JSON
  escapes astral characters *as* surrogate pairs, so a decoder recombines two
  surrogate code points into one astral character: 2 in, 1 out. CPython's own
  `Lib/test/datetimetester.py` contains a lone `\ud83d`. Strings containing
  surrogates therefore travel as base64 of `utf-8/surrogatepass`.

The second was **not** anticipated by reasoning about the schema; only running
the proof over real code surfaced it. That is the argument for §2.4.

### 2.4 Totality proof

A1 is done when, for every `.py` file in CPython's `Lib/`, parse → encode →
decode → `ast.dump(include_attributes=True)` compares equal, **on two targets**.
Comparing *with* attributes is deliberate: it proves `SourceLoc` survives,
which `-g` and every diagnostic depend on.

Files that do not parse under the target are excluded from the denominator
(`Lib/test` ships deliberate bad-syntax fixtures); they are reported, never
silently dropped.

### 2.5 Exhaustiveness — how I4 is *enforced*, not merely intended

Verified on this toolchain (g++ 13, `-std=c++20`): a `std::visit` over an
overload set that **omits an alternative is a hard compile error**
(substitution failure), not a warning. So a generated `std::variant` gives I4's
"adding a node kind must break the build" guarantee natively.

There is exactly one hole. A generic catch-all compiles and silently swallows:

```cpp
std::visit(ov{
    [](FunctionDef const& f){ ... },
    [](auto const&){ /* BANNED — swallows every future node kind */ },
}, stmt);
```

**Rule: no generic (`auto`/template) arm in any visitor over `pyc::ast` or
`pyc::ir` sum types.** A build-time lint enforces it. Handle the cases you
support and emit a §1.1 `Error` for the rest — explicitly, one arm each.

That single rule is what converts C++20's exhaustiveness from discipline into
a guarantee, and it is the reason C++ can satisfy I4 at all.

---

## 3. §3 — Lowering → Backend (A3 → A4)

Typed SSA, per function, basic blocks in a `Function`.

```cpp
namespace pyc::ir {

// The type lattice. Top = a boxed PyObject* of unknown type; everything below
// is a refinement A3 has PROVED. Backend may assume nothing A3 did not prove.
struct Type {
    enum class Kind { Top, Boxed, Int64, Float64, Bool, ExactType, Bottom };
    Kind kind = Kind::Top;
    PyTypeObjectRef exact;   // valid iff kind == ExactType
};

struct Value { uint32_t id; Type type; };

// Ownership is part of the type system, not a convention (I2/A2).
enum class Ownership { Owned, Borrowed };

struct Instr {
    Opcode              op;
    std::vector<Value>  args;
    std::optional<Value> result;
    Ownership           result_ownership;
    SourceLoc           loc;          // required: powers -g and diagnostics
    // Every unboxed op carries the boxed path it falls back to when its
    // guard fails. Unboxing without this is banned (I2).
    std::optional<BlockRef> deopt;
};
}
```

**Contract.** Any instruction whose `Type` is narrower than `Boxed` must carry
either a proof (from A3's dataflow) or a runtime guard plus a `deopt` edge to
an equivalent boxed computation. An unguarded narrowing is how
`math.factorial(25)` silently wrapped in the old tree.

**Errors are explicit edges**, following the C-API convention (`NULL` / `-1`
with the exception set) — not `setjmp`/`longjmp`. The old runtime's
jump-based frames are a documented source of frame-leak crashes and are not
carried forward.

---

## 4. §4 — Runtime / C-API binding (A2 → A3, A4)

**The value representation is CPython's `PyObject*`.** A2 defines no object
layout (CHARTER §2). Unboxed forms in §3 are local optimizations that must
always be re-boxable into a genuine `PyObject*`.

A2 exposes to A3/A4:

```cpp
namespace pyc::rt {
// The set of C-API entry points lowering may emit calls to, with their
// refcount contract encoded so A3 can reason about Ownership statically.
struct CApiSymbol {
    std::string   name;            // e.g. "PyNumber_Add"
    Ownership     returns;         // Owned for new refs, Borrowed otherwise
    bool          may_raise;       // if true, callers MUST have an error edge
};
const CApiSymbol* lookup(std::string_view name);

// Generated program entry: Py_Initialize, interpreter config, module setup,
// then the compiled module body. Owned by A2 because it is C-API sequencing.
void emit_entry_point(/* ... */);
}
```

**Rule.** Lowering never invents a C-API call. If a symbol is not in A2's
table with its refcount contract recorded, A3 may not emit it. This is what
keeps refcount discipline checkable rather than hopeful.

---

## 5. §5 — Driver CLI (A4 → A5, and the user)

The only interface A5 may depend on, and the only one users see.

```
pyc <source.py> [-o OUT]
    [--python=X.Y] [-std=pyX.Y] [--python-abi=...] [--python-sysroot=DIR]
    [-O0|-O1|-O2|-O3] [--static] [-g]
    [--emit-llvm] [--emit-asm|-S] [--verbose]
```

**Contract.**

- Exit `0` **iff** an executable was produced at `-o`.
- Any nonzero exit leaves no output file and prints §1.1 diagnostics to stderr.
- Diagnostics go to stderr; program output never does.
- `--python`/`-std` semantics are fixed by `VERSION_TARGETING.md`.

A5's `CompilerAdapter` targets exactly this. Changing it is an interface change
requiring A0 sign-off, because it silently invalidates every recorded baseline.

---

## 6. Deliberately unspecified

Left to each layer, so freezing these interfaces does not over-constrain:

- A1's internal deserialization strategy; the JSON schema behind §2.1.
- A3's lattice implementation, dataflow algorithm, and pass ordering.
- A4's LLVM construction, pass pipeline, and whether runtime bitcode is LTO'd
  (it should be — it is what lets C-API call overhead inline away).
- A2's caching of `CApiSymbol` lookups.
- Whole-program vs incremental compilation *strategy* — but the product goal
  (a single self-contained binary) means the driver's observable behaviour is
  whole-program: all reachable user modules land in one executable.

---

## Changelog

- **v1.1 — 2026-08-22.** Added §1.2 (tree depth), §2.2a (encoding
  hazards) and §2.4 (totality proof); renumbered exhaustiveness to §2.5.
  Additive only — no existing contract changed.
- **v1 — 2026-08-22.** Initial freeze. C++20; JSON parse boundary; `std::visit`
  exhaustiveness plus a catch-all ban as the I4 mechanism.
