# Unboxing

Keeping a Python `int` in a machine register instead of on the heap.

Written mid-implementation. Step 1 (the analysis) has landed; steps 2 and 3
have not. Every number here was measured on this machine against the target
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
2. **Tagged storage + unboxed arithmetic.** Candidates get the three slots;
   stores from an object try to unbox (exact int, fits in i64) so the existing
   iterator path benefits without waiting for step 3; loads as an object box on
   demand. Needs one runtime helper, `pyc_rt_unbox_int`, because the alternative
   is open-coding `PyLong_CheckExact` in LLVM, which means hardcoding the
   offset of `ob_type` -- an ABI guess this tree has no business making.
   Projected, not measured: ~35 -> ~11 ns/iter on `nested`, the residue being
   the iterator's own allocation.
3. **Native range loop**, carrying the guard above. Removes that residue.

Steps 2 and 3 are only worth their complexity together; step 2 alone leaves the
iterator allocating a `PyLong` per step.
