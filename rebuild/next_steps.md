# Next steps — performance (plan question A)

**Date:** 2026-11-23
**Status:** planning. No lock-file edits here. CHARTER §4 / I9 (a claim is
measured, or it is not made) governs every item.

The performance question from the roadmap was "what is next for
performance?" This file answers it as an ordered list, with the immediate
decision at the top. Correctness > completeness > performance
(STATUS.md priority): a faster pyc that disagrees with CPython is a wrong
program, and I2 (no semantic divergence for perf) is a hard floor on every
item below.

## Measured current state (2026-11-23)

- General unboxing is **done** — all 16 steps in `rebuild/UNBOXING.md`
  landed. `s += i*j` at `-O2`: 0.007s vs CPython 0.197s (C folds the O(1)
  form, so it is not the right bar for the local shape).
- The `sum(<single-clause genexpr>)` fusion (`try_lower_fused_genexp` →
  `lower_genexp_sum`, `lower.cpp:6729`) is **enabled and correct** — it
  matches the oracle at N=40/200/300 (matmul = 300×300 byte-identical).
- Fusion reaches ~1.0×; the loop-only sibling that takes the same captures
  as parameters and unboxes its `for k in range(N)` to a native int runs
  ~1.18× (STATUS.md 2026-11-18).
- Baseline `verify/perf/baseline.json` re-recorded as the **full set**
  (10 workloads, matmul included) on marvin — the first baseline that can
  show the fusion lever. Fusion-enabled record: `matmul` 1.14×, numeric
  1.26–1.33×, pure-python 1.16–1.21×, c-bound 1.00× (relative-to-oracle;
  the machine-invariant quantity). Prior `hal9k` record was smoke-only and
  predates fusion.

Full detail and the open question: `rebuild/STATUS.md`; the design for #1:
`rebuild/FUSION_UNBOXING.md`.

## 1. Fusion unboxing — the big lever (highest value, correct-risky)

**What:** `lower_genexp_sum` drains the synthetic `<sumcomp>` elt-loop via
boxed `PyObject_GetIter` + boxed `IterNext` and binds `k` with boxed
`store_target`, so every step allocates a `PyLong` for `k`. The win is to
drive that loop's iteration through the **native range path**
(`is_native_range_for` / `emit_for_range_loop`, which already exists) when
the genexpr iterates a builtin `range(...)` whose bounds fit i64 — so `k`
becomes a native i64 counter. The accumulator `.acc` stays boxed for this
slice.

**Design:** `rebuild/FUSION_UNBOXING.md`. Reuses existing machinery rather
than a second loop shape. I2 guards in that doc are non-negotiable:
range guard (I1), overflow deopt (`sadd.with.overflow` without a fit
proof), borrowed `.0` (the `sum(x for x in range(1000))` segfault), and
the nested-`sum` fallback.

**Correctness net (DONE WHEN):** `int_stress.py`, the fusion corpus,
`matmul` at 40/200/300, the two borrow/overflow crashers, and
`make verify fast --fail-on-silent-wrong` all stay green.

**Open decision — measure first, or go straight to the slice?** The
design's central hypothesis (the per-step `PyLong`-for-`k` alloc is the
dominant residual cost) is exactly a hypothesis: the flamegraph cannot
isolate the hot loop, and the speedup delta is all the evidence. The
disciplined first move is to **prove or disprove it before touching the
lock file** (Architect role: draft, do not implement). Concretely, add a
temporary boxed-vs-native counter toggle (or read the matmul
flamegraph's top contributors) and measure. If `k`'s alloc is not the
bottleneck, this effort changes direction rather than spending it. Go
straight to the smallest slice only if that is preferred.

## 2. Fill the empty `real-app` perf class
**Status: DONE (2026-11-28).** `verify/perf.py` declares `REAL_APP`; two
workloads now use it:

- `json_validate` — runs the real, dependency-free third-party package
  `fastjsonschema` (vendored under `verify/corpus/realapp/`) to validate a
  deterministically generated dataset; the validator it generates at
  runtime also runs under pyc.
- `host_encode` — runs the real, dependency-free package `idna` (vendored
  beside it) to encode N host names to IDNA/punycode.

Both are pinned, deterministic, no clock/rand/network (perf.py docstring,
I9). The vendored source is folded into the set pin via the new
`Workload.pinned_files` field + `pin_hash` extension, so editing the
third-party code changes the set identity like an input edit would. They
were sized to ~1.5 s of work on the oracle and validated byte-identical
under pyc vs CPython.

This was the *only* named class the committed full-set baseline
(`verify/perf/baseline.json`, matmul included) did not exercise. It is
also where the fusion lever (#1) gets validated on genexpr-dense code,
since no named workload exercises that shape heavily. Once #1 proves the
native-range counter win, a genexpr-dense real-app workload is the direct
target to show it — so #2 and the genexpr-dense recheck in #4 are one
effort, expressed in two places.

## 3. Characterise perf variance → turn the nightly gate into a real gate
**Status: DONE (2026-11-28).**

The nightly `perf` (full set, `metric.yml`) was warning-only: drift beyond
±15% was a red warning, not a failure, "until run-to-run variance is
characterised". That is now done.

- `verify/perf_variance.py` characterises the per-workload and per-class
  run-to-run spread of the **speedup** (oracle/pyc median — the
  machine-invariant quantity; STATUS.md documents it moving ±5% across
  machines vs ±49% in absolute seconds). It gates on
  `max(absolute_floor, spread_multiple × robust_CV)`: `absolute_floor`
  (0.02) keeps trivially-stable c-bound workloads from gating at a
  sub-percent number, and the **robust** CV (1.4826 × MAD/median, not plain
  std) keeps one environmental blip from setting the gate. Both knobs are
  passed in and recorded in the file, so the number is auditable (I9).
- `verify/perf/variance.json` commits the current model (smoke set, 5 runs,
  marvin 64c). The gate (`perf_compare --variance`) fails a workload when
  its current speedup falls more than its threshold below the baseline.
  `perf_compare` still defaults to warning-only (exit 0) with no
  `--variance`; the gate is opt-in and CI-turns-on via the workflows.
- The model is **run-specific** — it is what it was measured on. CI's own
  quiet runner differs from this shared dev box, so re-characterise on CI
  (or in place) after any runner/flag change. The per-workload numbers
  transfer across host classes only because the gate compares speedups,
  never absolute time.

Depends on nothing; ran in parallel with #1.

## 4. Later slices (after the immediate work)

- Unbox the `<sumcomp>` accumulator — only after its elt's int arithmetic
  is itself routed through the unboxed path (not the matmul shape; a later
  slice, per FUSION_UNBOXING.md).
- Re-check whether the nightly set should add a genexpr-dense workload
  (see #2) so the fusion lever has a direct target.

## Ordering

Do **#1** (after the measure-first call) — the highest value and the
direct continuation of the unboxing work. **#3** is done; it was the
parallelisable work that turned the perf gate into a real gate (a variance
model committed at `verify/perf/variance.json`). **#2** needs the harness
+ a pinned corpus but is independent of #1/#3. None of #1–#4 is a quick
continue; #1 is a dedicated, correct-risky effort.
