# Unboxing

Keeping a Python `int` in a machine register instead of on the heap.

Written mid-implementation. Steps 1–3 have landed. Every number here was
measured on this machine against the target
sysroot `cp314-3.14.7-tier1`, and the commands are in the session that produced
them. Nothing in this file is a projection unless it says so.

## Why, and why not the cheaper thing

The task began as "unbox the arithmetic". The first measurement overturned
that: pyc was already at or ahead of CPython on every local arithmetic shape,
so there was nothing to win *against CPython*. That was the wrong bar. Against
the machine, a nested `i*j` loop cost 0.136s where the same loop in C costs
0.001s.

Two things then turned out to sit in front of unboxing, and both have landed:

* Every global read and write called `PyImport_AddModule("__main__")` first
  (`eca2260`). Global `int +=` 0.560s -> 0.134s.
* Refcounts and local-slot access were out-of-line calls -- not merely their
  own cost, but barriers the optimiser could not move anything across
  (`c2ad7df`). 36.7 ns/iter -> 11.3 ns/iter on an otherwise identical loop.

With those gone, the remaining per-iteration cost is genuine allocation and
dispatch, which is what unboxing removes -- and it is now visible to LLVM,
which is what unboxing depends on.

Modelling one iteration of `s += i * 3` against the target's own libpython:

| | ns/iter |
|---|---|
| boxed, refcounts inlined (what pyc emits today) | 46.3 |
| fused expression: guard operands, compute the tree in i64, **one** allocation | 22.1 |
| accumulator never becomes an object at all | 0.3 |

Expression fusion is the tempting slice, and it buys about 2x, because what
remains *is* the allocation and the guards. Essentially all of the win requires
the value to stay in a register. That is the whole argument for doing the hard
version.

## The representation

Three slots per unboxed local, as LLVM allocas -- **not** in the `%locals`
frame array. A local that the analysis accepts is provably uncaptured, so
nothing outside the function can observe it, and keeping it out of the frame
leaves `pyc_rt_make_function`, closures and the call path untouched.

    %x.v : i64    the machine value
    %x.b : ptr    the boxed fallback
    %x.s : i8     0 = unbound, 1 = value is in .v, 2 = value is the object in .b

`unbound` is a real state, not an optimisation: `if c: x = 1` then `x` must
still raise `UnboundLocalError`.

The load-bearing question was whether that discriminant and fallback defeat
register allocation, since a design that spills is worth nothing. Measured on a
C model emitting exactly this shape, guard and deopt included:

| | ns/iter |
|---|---|
| pyc today, nested `i*j` | ~35 |
| tagged repr, guard + deopt | **1.7** |
| pure unboxed, no guard or fallback | 0.3 |

The guard and fallback cost ~1.4 ns and the hot loop stays in registers
(`imulq`/`addq` with overflow branches, no spills). The design survives.

## Overflow

Python ints are unbounded. The scar this has to answer to is recorded on
`ir::Op::ConstInt`: *materialising a literal into a machine word is how the old
tree wrapped factorial(25)*.

So overflow is never assumed away. Arithmetic uses the checked intrinsics
(`llvm.sadd/ssub/smul.with.overflow`), and on overflow the operation deopts:
box both operands, redo it through the C-API, and store the result boxed
(`.s = 2`). The local stays boxed from then on and every later operation on it
takes the slow path -- correct, and slow in exactly the case Python is slow.

`IntAddOvf` and friends carry two successor blocks, the way `IterNext` already
does (`%7 = iter.next %5 -> body bb5, done bb6`). That precedent is why this
fits the IR rather than needing a multi-result value.

## The `range` obligation

The analysis types the target of `for i in range(...)` on **syntax alone**.
Whether `range` is the builtin is not statically decidable -- the name can be
rebound, including between two executions of the same loop.

This is not optional bookkeeping. Excluding `for` targets left 6 candidates in
1192 locals across the language corpus (0.5%), and selected nothing in any
loop: an untyped `i` cascades, `s += i * 3` stops typing, and the accumulator
goes with it. Typing it takes the arithmetic benchmarks to full selection
(`nested: i* j* s*`).

**The lowering therefore owes a runtime guard** at loop entry: the callee is
the builtin `range`, and the arguments are exact ints that fit in i64. When the
guard fails the loop must run boxed. An unguarded lowering of this is a silent
wrong answer of exactly the kind CHARTER I1 forbids. The obligation is written
at the `For` arm in `scope.cpp` so it cannot be quietly skipped, and it must
land in the same commit as the native range loop.

The guard does not require duplicating the loop body. Both paths feed one body
through the tagged slot:

    head:
      if (native) { if (ctr >= stop) goto done; i.v = ctr; i.s = 1; ctr += step; }
      else        { item = PyIter_Next(it); if (!item) goto done; i.b = item; i.s = 2; }
      goto body

`native` is loop-invariant, so LLVM unswitches it and the fast version comes
out clean.

## What the analysis rejects, and why each one matters

`int_locals()` is deliberately over-restrictive, on the reasoning `scope.cpp`
already states for cells: a name it misses costs speed, a name it takes wrongly
is a silent wrong answer.

* **bool** -- `bool` is an `int` subclass. `type(True & True)` is `bool`, and
  unboxing `True` to 1 erases that.
* **`/` and `**`** -- not closed over the ints. `6 / 2` is a float, `2 ** -1`
  is 0.5, and `2 ** 64` is unbounded from a tiny input.
* **params** -- they arrive as objects; unboxing one means proving every caller
  passes an int, which is not a function-local question.
* **capture, `global`, `nonlocal`** -- the value lives in a cell something else
  reads through.
* **`locals` / `vars` / `eval` / `exec` / `dir`** -- they read the frame. Tagged
  slots live outside it, so a function that mentions any of these keeps every
  local boxed.
* **`del`** -- needs an unbound transition this does not model.
* **unpacking, `except as`, `with as`, match captures, imports, defs** -- bind
  values the analysis cannot type.

Validated on 12 adversarial cases: the good case selects, every rejection case
selects nothing.

## The net

`verify/corpus/language/int_stress.py` is the semantic net any implementation
must keep passing: 40 adversarial integer behaviours -- overflow in every
operator that can overflow, floor division and modulo rounding toward negative
infinity, shift edge cases, small-int identity (`256 is 256` is observable),
bool/int interaction, reflected operands, `hash(2) == hash(2.0)` and dict-key
agreement across the int/float boundary. Byte-identical to CPython before any
of this work, which is what makes it a net rather than a hope.

Per the practice in `feedback_pyc`: the stress file is written and diffed
against the target **before** the feature, because reasonable-looking output is
where divergence hides.

## Staging

1. **Analysis** (`876b8fc`, landed). Byte-identical LLVM on all 756 language
   corpus files -- nothing consumes it yet, which is the point: it could be
   validated before anything depended on it. `PYC_DUMP_INTLOCALS` reports the
   per-function selection.
2. **Tagged storage + unboxed arithmetic** (landed). Candidates get the three
   slots; stores from an object try to unbox (exact int, fits in i64) so the
   existing iterator path benefits without waiting for step 3; loads as an
   object box on demand. Overflow deopts to the C-API. `pyc_rt_unbox_int`
   exists because open-coding `PyLong_CheckExact` would hardcode `ob_type`.
3. **Native range loop** (landed), carrying the guard above.
4. **Native compare** (landed). `if`/`while`/`assert` and compare *values*
   (`x = a < b`) of two `int_locals` or constants use `icmp` on i64.
   The result of a value compare is boxed with `PyBool_FromLong`, never as
   an int -- `type(a < b)` is `bool`. `is`/`in` stay boxed (identity and
   containment are not i64). Mixed int/float and bool operands stay boxed.
   Deopt is the existing `PyObject_RichCompare` path.
5. **While-loop i64 phis** (landed). A `while` whose body has no nested
   `def`/`class` carries live int_locals in SSA phis at the head. Overflow
   deopts to a boxed clone of the loop so the fast path has one predecessor
   and the add result dominates the latch. Carrying a pre-loop `s = 0` into
   a loop without a phi at that loop is a silent wrong answer (`s += i*j`
   stuck on 0). `IntStore` of an already unboxed slot writes only `.v`
   (no `decref`).
6. **for-range i64 phis** (landed). Same `can_i64_phis` as while, shared
   native/boxed body (not unswitch). Accumulators are phis at the head;
   the range target stays a tagged slot. Overflow deopts to a boxed clone
   that continues the **same** `rid` / iterator -- restarting `range()`
   is a silent extra trip. Nested `for i: for j: s += i*j` phis `s` and
   `i` on the inner loop.
7. **Read-only param unbox at phi-loop entry** (landed). Params are not
   `int_locals` (they arrive as objects). A `while i < n` whose `n` is a
   parameter therefore boxed every compare -- nested `s += i*j` 0.056s
   vs 0.005s with a local `n`. At loop entry, a parameter that the body
   does not assign is `IntUnbox`'d into the live i64 map (fail → boxed
   clone). `True`/`"x"` stay boxed (`PyLong_CheckExact`). Do not IntLoad
   names that appear only in the `while` test: `while (n := 3)` is still
   unbound at entry (`walrus_scopes.py`).
8. **i64 phis at `if` joins** (landed). Each `endif` merges live i64s that
   both arms still hold. Names assigned on only one arm drop out (next
   use IntLoads). Overflow deopt continues every stacked body (then/else,
   then the loop), not only the innermost. `if False: s = 5` then `s + 1`
   is 1, not 6.
9. **`continue` / `break` / `return` in phi-loops** (landed). `continue`
   is an extra predecessor of the loop-head phis with the live i64s at
   the jump. `break`/`return` do not feed the latch; if the body is
   terminated, the latch incoming is omitted.
10. **Nested-loop rejoin** (landed). An outer phi-loop may contain inner
    `while` / native for-range whose bodies are themselves phiable.
     Inner phis do not dominate the outer latch, so the inner exit
     IntLoads updated names from tagged slots. Names assigned in a loop
     body and not live-in are not preloaded (avoids UnboundLocal on
     `j = 0` inside). `match` still refuses.
11. **GIL-free innermost phi-`while`** (landed). A `while` whose body is
    phiable, has no nested loop, and has no Call/attribute/etc. drops the
    GIL for the body (`PyEval_SaveThread`) and reacquires on latch,
    `break`/`continue`/`return`, deopt, and any incref/decref/unbox.
    for-range is not eligible (shared boxed `IterNext`). This is a
    concurrency cut, not the 7× vs C (that residue is still `jo`).
    `try`/`with`/`for` in the body also refuse GIL-free (C-API).
12. **`try`/`with` rejoin** (landed). A phi-loop may contain `try`/`with`.
    Exception and `__exit__` joins do not dominate the latch, so live i64s
    are IntLoaded from tagged slots at `except`/`finally`/`with.after`.
    `match` / `TryStar` / `AsyncWith` still refuse.
13. **Generic-`for` phis** (landed). `for x in xs` (not native range) uses
    the same head phis as `while`. The iterator and target stay boxed;
    accumulators are phis. Overflow deopts to a boxed clone that continues
    the **same** iterator. Clone must not `frame_owned_.pop` the GetIter
    already popped on the fast done path.

Steps 2 and 3 landed together: step 2 alone leaves the iterator allocating a
`PyLong` per step. Step 4 closes the `while i < n` hole that still boxed
every iteration. Step 5 is the while accumulator. Step 6 is the for-range
accumulator. Step 7 is `while i < n` when `n` is a parameter. `verify fast`
766/766 at `-O0`, no new P0.

Measured on this machine, `n = 2000` (4e6 iters of `s += i * j`), `-O2`:

| | s |
|---|---|
| CPython | 0.197 (for) / 0.269 (while) |
| pyc, for-range (steps 2–3) | 0.024 |
| pyc, `while i < n` before step 4 | 0.069 |
| pyc, `while i < n` after step 4 | 0.022 |
| pyc, `while i < n` after step 5 | 0.016 |
| pyc, for-range after step 5 | 0.018 |
| pyc, for-range after step 6, `-O0` | 0.012 (was 0.018 on this machine) |
| pyc, for-range after step 6, `-O2` | 0.007 (same as step 5 here; LLVM already promoted tagged slots) |
| pyc, `while i < n` param, before step 7 | 0.056 |
| pyc, `while i < n` param, after step 7 | 0.005 |
| C | 0.001 |

The residue vs C is still overflow `jo` and the range-target IntLoad.
GIL-free (step 11) is a concurrency cut on innermost phi-`while` bodies;
it is not expected to close the 7× vs C.

i64 phis are refused when the body has `match` / `TryStar` / `AsyncWith`.
`try`/`with` and generic `for` rejoin via IntLoad (steps 12–13). Overflow
in a phi loop redoes the current op through the C-API before switching to
the boxed clone -- skipping it was a silent wrong add.

**for-range unswitch was attempted and reverted.** Unswitching native vs
boxed into two loops made `for _ in range(10**9)` interrupted by SIGALRM
SIGSEGV on shutdown (~half the runs, `loop_periodic.py`). Shared body
through the tagged slot (step 3) does not. Step 6 keeps that shared body
and puts accumulator phis at the head. Do not retry unswitch without an
unwind story for the native path.
