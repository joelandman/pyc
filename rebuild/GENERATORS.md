# Generators (A3)

Status: **design validated, implementation in progress.** Every mechanism below
was exercised end to end against CPython 3.14.7 before any C++ was written.

## Decision

Generator bodies -- generator expressions first, then `yield` and `async` --
are compiled by **CPython at build time** into code objects, marshalled into
the binary, and run through the interpreter that is already linked in.

pyc does not implement suspension. It supplies the two things the code object
needs from its enclosing scope: the eagerly-evaluated outer iterator, and a
closure tuple of cells.

## Why not the alternatives

**Chunked materialization (the 2026-07-17 plan) is RETIRED.** It violates the
charter this tree is built on:

- I1: running ahead raises exceptions CPython never raises. A body that fails
  on element 5 raises even when the consumer asks for only 2.
- I2: side effects happen before the consumer requests them. Directly
  observable, and measured: `next()` on a genexp over a printing source prints
  exactly one line.
- Generators over blocking sources (sockets, queues) read ahead and hang.

Retired explicitly on 2026-08-23 once the conflict was surfaced. Reintroducing
it needs a recorded charter exception, not a quiet revival.

**A native iterator type** (a state machine over the loop nest) is tractable --
`yield` inside a generator expression is a SyntaxError, so the control flow is
a fixed nest of for/if clauses, not arbitrary suspension. But the object would
not BE a generator: `type(g).__name__`, `isinstance(g, types.GeneratorType)`
and `inspect.isgenerator(g)` all diverge. It also does nothing for `yield`.
Worth revisiting later as an optimisation, gated by differential tests proving
it agrees with this one.

**Real coroutines** (LLVM `llvm.coro.*`, or a CPS transform) are the eventual
answer for compiled-speed generators, and are what general `yield` needs
regardless. Much larger. Not first.

The cost of this design is that generator bodies run at CPython speed. That is
not a regression against "the program does not compile at all".

## Semantics that must be reproduced

Each was measured, not assumed:

| Property | Behaviour |
|---|---|
| Free variables | **Late-bound through cells.** `n=1; g=(x*n for x in [1,2]); n=10` yields `[10,20]`. Capture by value is wrong. |
| Outermost iterable | **Evaluated eagerly at creation**, and its exception surfaces there, not at first `next()`. |
| Inner iterables | Lazy, re-evaluated per outer element. |
| `.0` | The genexp code object receives the **iterator**, not the iterable. |
| Loop variable | Does not leak into the enclosing scope. |
| Object identity | Really a `generator`: `send`/`throw`/`close`, `gi_frame`, `__name__ == '<genexpr>'`. |

## Build side (`pyc_parse`)

1. Run `symtable` on the module. For each genexp scope it reports `get_frees()`
   -- CPython's own scope analysis, so pyc never re-derives which names are
   free. It correctly separates enclosing-function locals (free) from globals
   (resolved through `__globals__`, no cell).
2. For each `GeneratorExp` node in source order, replace `generators[0].iter`
   with a placeholder name, unparse, and compile a wrapper:
   `def __w(<frees>): return (<genexp>)`. The inner `<genexpr>` code object has
   exactly the right `co_freevars` and `co_varnames == ('.0', ...)`.
3. Marshal it, base64 it, attach it to the node in the JSON envelope along with
   the freevar names.

AST nodes and symtable scopes are matched by source order, with linenos
asserted to agree: a mismatch must be an error, never a silently wrong closure.

## Run side

Lowering emits, per generator expression:

1. Evaluate the outer iterable and `PyObject_GetIter` it -- eagerly, so an
   exception surfaces at creation as CPython does.
2. Load a cell for each freevar from the enclosing scope. pyc's existing
   closure analysis already forces a local read by a nested scope into a cell,
   and it counts generator expressions as nested reads. A freevar with no cell
   is a compile error, not a guess.
3. Hand blob, closure tuple and iterator to one runtime helper, which
   unmarshals the code object **once** (cached per call site), builds a
   function over pyc's module globals, sets the closure, and calls it.

## Validation performed before implementing

- marshal -> `PyFunction_New` -> call yields a real `generator`;
  `inspect.isgenerator` true.
- Mutating a cell after creation changes later values: late binding works.
- Nested for-clauses and globals resolve correctly.
- Laziness preserved: creating a genexp over a printing source prints nothing.
- 191 bytes of marshalled code for a one-clause genexp.

## What the mechanism actually covers (measured 2026-08-23)

The design was written for generator expressions, but the same three steps
(build-time compile → marshal → `PyFunction_New` at runtime) turned out to
carry every suspendable body, which is why it was chosen over a genexp-only
scheme. All of the following are implemented and differentially verified:

| construct | resulting object | code-object flag |
|---|---|---|
| generator expression | `generator` | `CO_GENERATOR` (0x20) |
| `def` containing `yield` / `yield from` | `generator` | `CO_GENERATOR` |
| `async def` | `coroutine` | `CO_COROUTINE` (0x80) |
| `async def` containing `yield` | `async_generator` | `CO_ASYNC_GENERATOR` (0x200) |

`await`, `async for` and `async with` need no separate machinery: they occur
only inside a body that is already handed to CPython.

Two things the build side must get right, both learned by getting them wrong:

- **Generator detection is scope-sensitive.** A `yield` belongs to the nearest
  enclosing function, so the walk must stop descending at every new scope. An
  early version did apply the scope skip inside nested functions but not to
  top-level statements, and marked plain functions as generators. It was caught
  at build time only because the compiled code object is asserted to carry
  `CO_GENERATOR` — the assertion was worth more than the review that missed it.
- **Qualnames must be repaired.** Compiling the body inside a wrapper gives it
  the wrapper's qualname; `code.replace(co_qualname=...)` puts back the name the
  source implies, which is what tracebacks and `repr()` show.

Scope analysis is `symtable.get_frees()` rather than a hand-rolled walk,
because it is the only thing that reliably distinguishes an enclosing
function's local from a module global.

## Cost, stated plainly

These bodies run at CPython speed. That is the accepted trade: the alternative
was not "a faster generator" but "the program does not compile". Replacing this
with a native state machine is an optimisation for later, and per I2 it may
only land behind differential tests proving the two agree observably.
