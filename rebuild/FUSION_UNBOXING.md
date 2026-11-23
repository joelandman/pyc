# Fusion unboxing — design (plan question A)

**Status:** design only. Not implemented. Touching `lower.cpp` here is a
correct-risky, separate effort (STATUS.md 2026-11-18); the architect drafts,
the SWE-compiler implements, and every claim is measured before it is stated.

## The measured gap

`sum(<single-clause genexpr>)` (the matmul shape `sum(A[i][k] * B[k][j]
for k in range(N))`) is natively fused by
`try_lower_fused_genexp` -> `lower_genexp_sum` (`lower.cpp:6729`). Fusion was
re-enabled 2026-11-18 after a disable-era regression; two fixes (read-capture
deref snapshot at `<sumcomp>` entry, loop var `k` a plain local unless a
nested lambda captures it) brought it from a 0.967x regression to ~1.0x.

Current number on marvin (64c, fusion **enabled**, `-O2`, from the
2026-11-22 full-set baseline): `matmul` **1.14x**. The loop-only sibling that
takes the same captures as parameters and unboxes its `for k in range(N)` to a
native int runs **~1.18x** (STATUS.md). The residual 0.04-0.18x is that the
synthetic `<sumcomp>` iterator loop **cannot unbox `k`**: it drains via
`PyObject_GetIter` + boxed `IterNext` and binds `k` with boxed `store_target`,
so every step allocates a `PyLong` for `k` and the accumulator does a boxed
`PyNumber_Add`.

Hypothesis to test before coding: the per-step `PyLong` allocation for `k`
(and the boxed add) is the dominant residual cost. If the flamegraph/trace
cannot isolate it (the trace is blind to native hot loops, STATUS.md), the
hypothesis stands only as a hypothesis and the speedup delta is the only
evidence.

## The change

When `lower_genexp_sum` can *see* that the genexpr iterates a builtin
`range(...)` whose bounds fit i64, drive the synthetic loop's iteration through
the **native range path** instead of the generic `IterNext` drain, reusing
`is_native_range_for` / `lower_for_range` / `emit_for_range_loop` rather than
writing a second loop shape:

1. At the point `lower_genexp_sum` currently does
   `lower_expr(*g.iter)`, ask `is_native_range_for`-style: is `g.iter` a
   `range(...)` Call with 1-3 non-star int args, target typed by `int_locals`?
   If no, take the existing boxed `IterNext` path (unchanged) — the fallback
   must stay correct for `range` rebound, non-int bounds, multi-arg ranges that
   overflow, and every non-range iterable.
2. Emit the `RangeGuard` + native/boxed cond-branch + native counter that
   `lower_for_range` already emits, then reuse `emit_for_range_loop` for the
   shared head/body/done structure. The synthetic `k` becomes the loop's i64
   target (or a tagged slot), which is the measured win.
3. The accumulator `.acc` stays boxed for now (its elt comes from a boxed
   subscript chain); unboxing it is a later slice, not this one.

## I2 guards (must not be silently skipped)

- **Range guard.** Whether `range` is the builtin is not statically decidable
  (the name can be rebound, including between two runs of the same loop). A
  `RangeGuard` that fails routes the loop to the boxed path. An unguarded
  native lowering of this is exactly the silent wrong answer CHARTER I1 forbids
  (UNBOXING.md "the `range` obligation").
- **Overflow deopt.** `range` bounds that do not fit i64 deopt to the boxed
  clone and continue the **same** iterator (restarting `range()` is a silent
  extra trip). `i64` increments stay `sadd.with.overflow` unless `c+1` fits is
  proven — do not `nsw` without a fit proof (UB / silent wrap for large `n`).
- **Borrowed iterator.** `.0` is BORROWED, owned by the outer frame
  (defect-1 fix). The native head must not claim ownership or `emit_decref` it
  on the done path — that was the `sum(x for x in range(1000))` segfault.
- **Nested `sum`.** `in_fused_loop_` still forces the real-generator fallback
  for a `sum` nested inside an outer fused-sum loop (a synthetic function
  cannot be created inside the outer loop's unwind pads without a dominator
  violation).

## Correctness net (DONE WHEN)

- `verify/corpus/language/int_stress.py` still byte-identical (the unboxing
  net any int work must keep passing).
- `verify/corpus/language/` fusion corpus (`genexpr_fusion_builtin` /
  `_scoping` / `_object_semantics`) byte-identical.
- `matmul` matches the oracle at N=40/200/300 (and the `sum(...,start)` /
  step / nested-sum edge suite the current dispatch already gates on).
- `sum(x for x in range(1000))` and `range(0, 2**63)` do not segfault or wrap
  (the two borrow/overflow crashers).
- `make verify fast` still green (`--fail-on-silent-wrong`) — this is the gate
  that would catch a silent regression.

## Recommended slice

Smallest verifiable increment: **range detection + native counter for `k`,
accumulator stays boxed.** That closes most of the measured gap (the per-step
`PyLong` for `k`) without touching the accumulator's boxed add. Unboxing the
accumulator (elt stays boxed) is a separate, later slice — and only after the
elt's int arithmetic is itself routed through the unboxed path, which is not
the matmul shape.
