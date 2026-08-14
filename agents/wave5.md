# Wave 5 — Close-out

Operating plan. Same roles as Waves 0–4: Coordinator (this conversation)
owns tickets, tests, docs, git; SWE implements one ticket; SWR is
read-only plus `ISSUES.md`. Nothing starts until you approve it.

Goal: empty **Open** of every leftover that is a crash, wrong-answer, or
a small documented limitation we can finish. Not “implement Python.”

---

## What “close” means

| Outcome | When |
|---|---|
| **fixed** | Binary matches CPython on the ticket cases at `-O0` and `-O2` |
| **wontfix** | Architectural, or the original plan already deferred it; register says why |
| **accepted** | Design done; implementation is its own later ticket (only if you reject shipping it here) |

A green `make check` is not proof. Same gates as coordinator.md.

Hard rules unchanged: one slice, one writer of `Compiler.cpp` /
`Runtime.cpp` / `Codegen.cpp`, tests first, commit each slice, trust
the executable.

---

## Inventory (every open ID)

### Track A — Wrong-answer leftovers (must fix)

Small, evidence-backed, no new architecture.

| ID | One-line | Lock |
|---|---|---|
| I-021 | NUL in str literals / `chr(0)` truncated | Parser + Codegen + Runtime |
| I-022 | KeyError / `Path` print not `repr` | Runtime |
| I-023 | Dynamic `*args` missing required → None | Compiler + Runtime |
| I-024 | Indirect `g(**{})` binds `{}` as positional | Codegen + Compiler |
| I-025 | TypeError name is IR / not `__qualname__` | Compiler + Codegen |
| I-026 | `del` tuple/str/dict slice is a no-op | Runtime |
| I-027 | `Pyc_DelSlice` reverse underflow; step 0 silent | Runtime |
| I-028 | Dead OOM checks after `new` | Runtime |
| I-030 | Boxed-accepting arms steal user methods | Compiler + Runtime table |
| I-031 | `fromkeys` / `os.path` miss alias / from-import | Compiler |
| I-032 | leftover `.get` on `m=os` / `C.get` / `sys.get` | Compiler + Runtime |
| I-033 | Adapter default slot uses param index | Codegen |
| I-034 | `os.keys`/`items`/`values`/`pop` are dict methods | Compiler |
| I-035 | Class-method defaults lowered in wrong scope | Compiler |
| I-036 | Traceback snapshot overwritten on reraise | Runtime |
| I-037 | Traceback `in` names are IR names | Codegen (+ Compiler names) |
| I-039 | `super().__init__` return prints `False` | Runtime |
| I-040 | `partition(None)` is ValueError, not TypeError | Runtime |
| I-041 | `format` miss prints `None` | Runtime |
| I-042 | `format` splits on `:`/`!` inside `[…]` | Runtime |
| I-045 | `str.find` drops `end` | Compiler + Runtime |
| I-046 | Boxed `split(None)` is space-split | Runtime |
| I-047 | `None.bit_length` prints None | Runtime |
| I-048 | Boxed `super().count` is tuple | Runtime |

### Track B — Finishable limitations

| ID | One-line | Lock |
|---|---|---|
| I-012 | Function `__name__` / `__doc__` / `__call__` on `Pyc_GetAttr` | Runtime (+ Compiler for `__doc__` if we store it) |
| I-018 | `sorted(..., reverse=)` + `cmp_to_key` | Runtime |
| I-020 | Kwargs dropped by method fallback | Compiler |
| I-038 | `super().__str__` on Exception subclasses | Runtime |
| I-043 | GDB printer: real DI struct + tag-7 heuristic | Codegen + `tools/pyc_gdb.py` |
| I-044 | Optional `-g` on `runtime.bc` | CMake |
| I-049 | Guard / comment: no function-local C++ ctors in Runtime bitcode | Runtime comment + AGENTS.md |

I-038 **list/dict subclassing** (`class L(list)`) is **not** this ID’s
fix. Instances are dict-backed. That is I-013-class layout, not
`SuperMethod`. Close I-038 for Exception `__str__` / other builtin
exception methods; file list/dict inheritance as wontfix or a new
design if you want it later.

### Track C — Designed or architectural (close as wontfix / accepted, not sneak-coded)

| ID | Close as | Why |
|---|---|---|
| I-011 | **wontfix** this wave | Full type objects were deferred in the original plan. Cheap `__name__` parse already works. |
| I-013 | **accepted** (design only) | Splitting tag 5/7 is a dedicated redesign of every type switch + Codegen GEP. I-047/I-048 are the concrete symptoms we *do* fix. |
| I-014 | **fixed** (W5.8) | Speculative unbox landed in Codegen. Native-join leftover is I-112. |
| I-016 | **wontfix** this wave | Arena / escape analysis is a new allocator. After I-014, not in a close-out. |
| I-017 | **wontfix** this wave | Subset expansions (µs/tz, Path multi-arg, hashlib.update, …) are feature work. Original plan: expand only with an explicit ticket. |

---

## Slices (serial)

Same cadence: ticket + failing tests → SWE + SWR → runner + `-O2` smoke
→ commit.

### W5.1 — Runtime leftovers, no Compiler lock

I-022, I-026, I-027, I-028, I-039, I-040, I-041, I-042, I-046, I-047, I-048.

One SWE, `Runtime.cpp` only. Grouped because they do not share a
control-flow hazard the way dispatch arms do; SWR still reviews each
behavior. If the diff is huge, split: strings/format (022/040–042/046)
then slice/exc/super (026–028/039/047–048).

### W5.2 — `str.find(..., end)` (I-045)

Compiler `find` arm + `PyString_Find4` + boxed handler. Small, but it
touches Compiler after W5.1 releases Runtime.

### W5.3 — NUL strings (I-021)

Parser `PyUnicode_AsUTF8AndSize`, Codegen length-explicit const,
`chr(0)` via `PyUnicode_FromStringAndSize`. Three files, one ticket.

### W5.4 — Call / defaults (I-023, I-024, I-025, I-033)

Compiler `*args` missing-check + Codegen adapter (`userLen` peel empty
kwargs; default-slot index; display name / qualname). I-037 can share
the same display-name table if cheap; otherwise I-037 waits for W5.6.

### W5.5 — Dispatch leftovers (Compiler locked, serial)

I-030, I-031, I-032, I-034, I-020. Same function as Wave 2
(`lowerMethodCall`). Do **not** parallelize. Pattern: proven type only;
runtime table for boxed builtins; modules are not dict method receivers.

### W5.6 — Class defaults + frames (I-035, I-036, I-037)

I-035 Compiler (lower defaults in `__module__` / class scope). I-036
Runtime snapshot-once. I-037 Codegen Python names (or leftover from
W5.4). Can be three tiny slices if locks conflict.

### W5.7 — Compat polish (I-012, I-018, I-038 `__str__`, I-043, I-044, I-049)

Function attrs, sorted+cmp_to_key, Exception `super().__str__`, GDB
composite DI + printer, `-g` on `runtime.bc` if it does not explode
binary size, AGENTS.md + comment for I-049.

### W5.8 — I-014 speculative unbox (optional, last)

Only if you approve Track C as “implement.” Codegen lock. Tag-check
unbox, native local slots, no globals, measure fibn / numeric_loop /
nbody. Abort the slice (revert) on a FILE_CASE or nbody mismatch.

---

## Explicitly still not Python

Unchanged from the original plan: `exec`/`eval`, compiling CPython
stdlib, JIT, TCO, `memoryview`, timezones, splitting the monolith.

---

## Order vs locks

```
W5.1 Runtime ──► W5.2 Compiler+Runtime ──► W5.3 Parser/Codegen/Runtime
                                          ──► W5.4 Compiler+Codegen
                                          ──► W5.5 Compiler (serial)
                                          ──► W5.6 mixed
                                          ──► W5.7 mixed
                                          ──► W5.8 Codegen (optional)
```

W5.1 can start immediately. W5.5 must not overlap any other Compiler
writer.

---

## Approval

Approve the whole wave, or approve Track A only (wrong-answers) and
leave Track B/C for later. Say whether **W5.8 / I-014** is in or out.
