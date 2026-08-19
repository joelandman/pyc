# Wave 12 — Leftover correctness + infra

Operating plan. Same roles as Waves 0–11: Coordinator owns tickets,
tests, docs, git; SWE implements one ticket; SWR is read-only plus
`ISSUES.md`. Nothing starts until you approve the slice.

Goal: empty **Open** of leftover crash/wrong-answer items, then the
small W11 leftovers. Not datetime redesign, not type objects, not
“implement Python.”

---

## What “done” means

| Outcome | When |
|---|---|
| **fixed** | Binary matches CPython on the ticket cases at `-O0` and `-O2` |
| **wontfix** | Architectural, or already deferred; register says why |
| **accepted** | Design only; implementation is a later ticket |

Hard rules unchanged: one slice, one writer of `Compiler.cpp` /
`Runtime.cpp` / `Codegen.cpp`, tests first, commit each slice, trust
the executable. After Runtime.cpp, rebuild both `libpycrt.a` and
`runtime.bc`.

---

## Inventory (every remaining open ID)

### Track A — Wrong-answers (must fix)

Small, evidence-backed, Runtime only.

| ID | One-line | Lock |
|---|---|---|
| I-217 | `deque(None)` is `[]`, not TypeError | Runtime |
| I-218 | set `\|` / `&` / `-` of None is silent | Runtime |
| I-219 | `cmp_to_key` factory `==` factory is True | Runtime |

### Track B — W11 leftovers

| ID | One-line | Lock |
|---|---|---|
| I-225 | `encoding=` still dropped (`readline(n)` already done) | Runtime (+ Compiler if `open(..., encoding=)` is compile-time) |
| I-228 | Bound `h = f.read; h()` is a token, no self | Runtime (+ maybe Compiler) |

I-228 needs bound-method objects (token + captured receiver). Same
class as `h = f.write`. Do **not** register `(2, "read")`. Cheap shape:
a type-2 dict `{"__self__": file, "__token__": "pyc_file_read"}` that
`Pyc_Apply` recognizes and prepends self. Full bound-method type is
I-013-class — out of scope.

### Track C — Latent (fix if cheap; otherwise file and move on)

| ID | One-line | Lock |
|---|---|---|
| I-112 | Native-join look-ahead misses `i64assign`; fallback unbox unguarded | Codegen |
| I-158 | Nested generator calls unwrapped; `list(int)` drains yield buffer | Compiler + Runtime |

### Track D — Infra

`-O2` / ThinLTO link fails on this machine: `LLVMgold.so` missing.
Import suite and user-default binaries cannot link. Not an ISSUES.md
semantic bug. Slice: find the gold plugin (or stop passing `-flto=thin`
when the plugin is absent) so `tests/o2_smoke.py` and the import suite
run here again.

### Track E — Designed / defer (not this wave unless approved)

| ID | Close as | Why |
|---|---|---|
| I-011 | **wontfix** this wave | Type objects. Cheap `__name__` parse already works. |
| I-013 | **accepted** | Splitting tag 5/7 is a dedicated redesign. |
| I-016 | **accepted** | Arena / escape analysis is Wave 4 leftover perf. After D. |
| I-017 | **accepted** | Umbrella. Remaining children I-113–I-115 only. |
| I-113 | **accepted** | Datetime layout (µs). |
| I-114 | **accepted** | Timezones. |
| I-115 | **accepted** | `datetime⊂date` needs I-011/I-013. |

---

## Slices (serial)

Same cadence: ticket + failing tests → SWE + SWR → runner + `-O2` if
the linker works → commit.

### W12.1 — Runtime wrong-answers

I-217, I-218, I-219. One SWE, `Runtime.cpp` only. `deque(None)` needs
a missing-vs-None sentinel (notes already say so — I-173 class). Set
ops TypeError on None. `cmp_to_key` factory `==` is AttributeError
(CPython 3.14), not dict eq.

### W12.2 — `open(..., encoding=)` (I-225 leftover)

Runtime `PyBuiltin_Open` + Compiler kwargs if `encoding=` is dropped
at the call site. Text mode only. Do not invent codecs; accept `utf-8`
/`locale.getpreferredencoding` no-ops that still store the name.

### W12.3 — Bound file methods (I-228)

Runtime: `Pyc_GetItem` on a `g_pycFiles` dict returns a bound wrapper;
`Pyc_Apply` prepends self. Same for write/readline/readlines/close.
SWR hunts the same token pattern on StringIO / hashobj / argparse.

### W12.4 — Latent (optional)

I-112 Codegen lock. Abort on FILE_CASE or nbody mismatch.
I-158 can be its own tiny Compiler slice after I-112 releases Codegen.

### W12.5 — `-O2` gold plugin (optional)

CMake / Compiler link line. No semantic change. Prove
`tests/o2_smoke.py` and one import-suite file link.

---

## Order vs locks

```
W12.1 Runtime ──► W12.2 Runtime (+ Compiler if encoding= is compile-time)
                 ──► W12.3 Runtime
                 ──► W12.4 Codegen / Compiler (optional)
                 ──► W12.5 CMake / Compiler link (optional)
```

W12.3 must not overlap another Runtime writer.

---

## W12.1 ticket (proposed first)

```
Title: W12.1 — leftover wrong-answers (I-217, I-218, I-219)
Goal: Match CPython on deque(None), set bitwise/None, and
  cmp_to_key factory identity compare.
In scope:
  - PyCollections_Deque: None → TypeError; 0-arg still empty
  - PySet_Union / Intersection / Difference: None → TypeError
  - factory==factory (no "obj" key) → AttributeError, not True
Out of scope:
  - I-225 encoding=, I-228 bound methods
  - I-173 empty min/max (already fixed); do not broaden sentinels
Files SWE may edit:
  src/runtime/Runtime.cpp
  (include/pyc/runtime.h only if a new helper is required)
Files SWE must not edit:
  src/Compiler.cpp, src/codegen/Codegen.cpp, tests/runner.py,
  FEATURES.md, ISSUES.md
Tests Coordinator already added: (insert before SWE launch)
Verify:
  - PYC_BINARY=./build/pyc python3 tests/runner.py
  - PYC_BINARY=./build/pyc python3 tests/o2_smoke.py
    (skip if LLVMgold still missing; note it)
CPython reference: 3.14 deque / set / functools.cmp_to_key
Related ISSUES.md ids: I-217 I-218 I-219
```

---

## Approval

Approve the whole wave, or Track A only. Say whether **W12.3 / I-228**
and **W12.5 gold** are in. W12.1 can start as soon as you say go.
