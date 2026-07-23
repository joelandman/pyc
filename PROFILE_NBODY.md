# nbody Profile Analysis (P0 unpack + P1 freelist)

**Setup:** `tests/nbody.py` `--opt=2`, energy matches CPython.

**Flamegraph:** [`nbody_flamegraph.svg`](nbody_flamegraph.svg)

---

## Wall-clock (execution only)

| Workload | Python | pyc `--opt=2` | Ratio |
|----------|-------:|--------------:|------:|
| nbody 50k | ~0.27 s | **~0.15 s** | **~1.8× faster** |
| nbody 500k | ~2.38 s | **~1.42 s** | **~1.7× faster** |

---

## `@advance` IR (after P0 unpack)

| Kind | Count |
|------|------:|
| `fadd` | 8 |
| `fsub` | 6 |
| `fmul` | 3 |
| `PyList_GetItemDouble` | 9 |
| `PyList_SetItemDouble` | 9 |
| `PyNumber_Multiply` | 12 |
| `PyList_GetItemObj` | 22 |
| `Py_DECREF` | 107 |

Native float ops and homogeneous list get/set are live on the velocity/position path.

---

## Profile hotspots (n=200k)

| Symbol | ~% |
|--------|---:|
| `Py_DECREF` | ~33% |
| `PyList_GetItemObj` | ~18% |
| `advance` leaf | ~rest of loop |
| `PyNumber_*` | reduced vs pre-P0 |
| **`malloc`/`free`** | **~0%** (P1 freelist) |

---

## What P0 unpack does (safe model)

1. **Layout tracking**
   - BODIES values → shared body layout `[list_float, list_float, float]`
   - `list(BODIES.values())` / `SYSTEM` → `structuredElementLayout`
   - `combinations(SYSTEM)` / `PAIRS` → `pairOfStructuredLayout`
   - Defaults `bodies=SYSTEM`, `pairs=PAIRS` bind layouts onto params **before** body lower
   - For-loop `PyBuiltin_List` + iter slot **copy layouts** (was dropping them)

2. **Unpack rules**
   - Pair item → each child gets body layout via `childStructuredLayout`
   - Body unpack: `pos`/`vel` → `list_float` + `knownFloatLists`; `mass` stays boxed
   - Nested `[x1,y1,z1]` from `list_float`: `GetItemObj` + `noteType(float)` so binops unbox
   - **Never** `GetItemDouble` on mixed body tuples (segfault / wrong physics)
   - **Never** put boxed unpack scalars into `numericFloatLocals` (refcount corruption)

3. **Stores**
   - `v1[i] op= …` / `r[i] op= …` use `GetItemDouble` + native op + `SetItemDouble` when handle is `list_float`

---

## P1 freelist

Thread-local freelists (cap 256) for plain int/float boxes in `Runtime.cpp`.  
Recycle only objects with empty container fields.

---

## Remaining headroom

1. More `fmul` — keep mass/mag on native float chain (`m1 * mag` still often `PyNumber_Multiply`).
2. Cut remaining `GetItemObj` on pair/body spine (still boxed structure walk).
3. Reduce `Py_DECREF` via fewer ephemeral boxes (escape analysis / stack floats).
4. Native `** -1.5` → `llvm.pow` / rsqrt on doubles.

### Regenerate flamegraph

```bash
./build/pyc tests/nbody.py -o /tmp/nbody_prof --opt=2
perf record -F 999 -g --call-graph dwarf,8192 -o /tmp/perf.data -- /tmp/nbody_prof 200000
perf script -i /tmp/perf.data | stackcollapse-perf.pl --all | flamegraph.pl > nbody_flamegraph.svg
```
