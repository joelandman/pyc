# pyc Compiler Review — Notes (paused, resumption plan inside)

## 1. The original request (verbatim, deduped)

> Please review the current state of the pyc python compiler. Please read the
> documentation markdown in depth. I want to compare our current state to our
> goals of correctness, completeness, and performance. I feel that the compiler
> is, in its present state a "toy" able to compile a small subset of python,
> while I am really aiming at the majority of python. I think something went
> wrong during the architecture and engineering process where this got lost.
> Please let me know if you concur with this hypothesis.

**Deliverable:** a gap analysis of current state vs. the three goals
(correctness / completeness / performance), plus a verdict on the hypothesis
that "majority of Python" intent was lost and the project narrowed to a small
subset.

## 2. Why the first attempt stalled

Dispatched 4 parallel `explore` subagents, one per doc group (IMPLEMENTATION,
README+FEATURES, ISSUES+FIXES, PLANs+PERFORMANCE). They looped/cancelled —
the files are too large to digest in a single subagent pass, and the
concurrent load made it worse.

**Fix for next time:** do NOT hand a whole big file to a subagent. Either:
- read chunks myself with `sed -n 'A,Bp' file`, or
- give subagents explicit line ranges (see plan below), one range per task,
  and fewer concurrent tasks (1–2, not 4).

## 3. Data already collected (verified, `wc -l`)

### Doc files (repo root)

| File | Lines | Role (per AGENTS.md) |
|---|---|---|
| session.md | 4324 | session history — skip or skim tail only |
| IMPLEMENTATION.md | 3127 | design + long bug-hunt log; most useful for "why is X like this" |
| ISSUES.md | 1900 | living register of open issues |
| OPTIMIZATION_PLAN.md | 775 | optimization goals/status |
| PERFORMANCE_BASELINE.md | 452 | perf numbers |
| DEBUGGING_PLAN.md | 441 | debugging goals |
| FEATURES.md | 355 | capability list |
| KnownGapsPlan.md | 219 | **directly relevant: known gaps vs. intent** |
| AGENTS.md | 196 | build/test/roles (already loaded into my context) |
| README.md | 140 | build/usage + scope statements |
| PERFORMANCE_OPT_LEVELS.md | 84 | O0/O2 behavior |
| FIXES.md | 75 | fix history (short) |
| PROFILE_NBODY.md | 44 | nbody profile (short) |

### Source tree (the architectural evidence)

```
src/frontend/PythonParser.cpp   1319   <-- the ENTIRE Python frontend
src/frontend/parse_helper.py     121   <-- Python helper?? needs investigation
src/ir/IR.cpp                    212
src/ir/LLVMDCE.cpp                29
src/Compiler.cpp              13437   <-- lowering (LoweringVisitor)
src/codegen/Codegen.cpp       4795
src/runtime/Runtime.cpp       18200   <-- boxed PyObject* runtime
src/runtime/MainWrapper.cpp     46
src/main.cpp                   109
```

**Key observation:** the frontend is 1,319 lines. A parser for "the majority
of Python" (CPython grammar: comprehensions, async, decorators, f-strings,
type hints, context managers, walrus, match, etc.) would be several thousand
lines even hand-written — and no grammar file or parser-generator artifact
exists in the tree. This strongly suggests a **hand-written recursive-descent
parser for a curated subset**, which would mechanically confirm the "toy"
hypothesis at the architecture level. Confirm by reading
`src/frontend/PythonParser.cpp` (it's small enough for one pass) and
`src/frontend/parse_helper.py`.

## 4. Resumption plan (ordered; each step is bounded)

### Step A — Frontend ground truth (highest value, ~15 min)
1. Read `src/frontend/PythonParser.cpp` in full (1319 lines — one pass is
   feasible; if not, two ranges: 1–700, 701–1319).
   - List exactly which statement/expression forms the `parse*` functions
     exist for (make a coverage checklist vs. CPython statement kinds).
   - Note anything NOT parseable: async/await? match? decorators?
     context managers? f-strings? type annotations? comprehensions?
     generators? default/keyword args? `*args`/`**kwargs`?
2. Read `src/frontend/parse_helper.py` (121 lines). What role does it play?
3. **Output:** a feature-coverage table: language feature | parsed? | notes.
   This alone largely answers the "completeness" goal question.

### Step B — IMPLEMENTATION.md in chunks (3127 lines; ~4 chunks)
Read with `sed -n` in these ranges, extracting per chunk:
- 1–300: Current Status header + architecture description.
  **Q:** how does it describe parsing? Any "subset" phrasing?
- 301–900: remaining design (AST/lowering/codegen/runtime).
  **Q:** runtime model, boxing, refcounting; opt-level notes.
- 901–2100: bug-hunt log (first half).
  **Q:** classify hunts (crash / wrong answer / coverage extension).
- 2101–3127: bug-hunt log (second half) + trailing sections.
  **Q:** same classification; any passage where stated goals conflict with
  actual scope; any perf numbers.
- **Output:** one-paragraph "what it claims to be" + bug-log trajectory
  summary + quote list of scope/intent passages.

### Step C — Goals & gaps docs (small, one pass each)
- `KnownGapsPlan.md` (219) — **Q:** what gaps are admitted? What's the target?
- `FEATURES.md` (355) — **Q:** full capability list + "not supported" list.
- `README.md` (140) — **Q:** quote scope statements verbatim.
- `DEBUGGING_PLAN.md` (441) + `OPTIMIZATION_PLAN.md` (775) — **Q:** stated
  goals vs. status achieved; which optimizations done vs. not.
- `PERFORMANCE_BASELINE.md` (452) + `PERFORMANCE_OPT_LEVELS.md` (84) +
  `PROFILE_NBODY.md` (44) — **Q:** all perf numbers, O0/O2 divergence.
- `FIXES.md` (75) — quick skim, trajectory.

### Step D — ISSUES.md in chunks (1900 lines; ~3 chunks)
- 1–700, 701–1400, 1401–1900.
- **Q per chunk:** which issues are OPEN? Classify: crash / wrong answer /
  missing language feature / perf. Count totals.
- **Output:** open-issue count + classification table; quote the top items,
  especially "missing language feature" ones (they map directly to the
  completeness gap).

### Step E — Synthesis (the actual deliverable)
Write `REVIEW.md` (or present inline) with:
1. **Current state** — one paragraph + the frontend coverage table.
2. **Gap analysis vs. goals** — three sections:
   - *Correctness:* open crashes/wrong-answer count (from Step D),
     bug-log trajectory (Step B), O0/O2 divergence (Step C).
   - *Completeness:* coverage table (Step A) vs. CPython feature set;
     KnownGapsPlan admissions (Step C); missing-feature issues (Step D).
   - *Performance:* numbers (Step C) vs. CPython/other baselines.
3. **Verdict on the hypothesis** — did "majority of Python" intent get lost?
   Evidence to weigh:
   - frontend line count + no grammar/parser generator (structural),
   - doc phrasing: "subset" vs. "majority" (from Steps B/C),
   - bug-log trajectory: fixing core machinery vs. extending coverage
     (Step B) — if the log is mostly coverage-extension, the intent was NOT
     lost, the execution just hasn't reached it yet; if it's mostly
     machinery, the project has been consolidating a narrow base.
4. **Recommendation** — options to consider (do NOT pre-decide; weigh in
   synthesis):
   - (a) keep the hand-written subset parser and expand it feature by feature
     (high cost: subset parsers rot as coverage grows),
   - (b) replace the frontend with a generated/full parser (e.g. a
     CPython-grammar bison/ANTLR/pycparser-style frontend feeding the same
     AST) — the lowering/codegen/runtime (26k lines) may be reusable,
   - (c) narrow the stated goal deliberately (accept "subset" as product).

## 5. Things already known from AGENTS.md (context, no re-read needed)

- Build: `cmake -S . -B build && make -C build check` (LLVM 22 on this
  machine; docs historically said 18). C++20, hard-coded `clang++`.
- Pipeline: Python source → AST → IR → LLVM IR → native exe; boxed `PyObject*`
  runtime with refcounting.
- 597 inline test cases in `tests/runner.py` CASES, compiled at `-O0` and
  compared against CPython output (hardcoded expected is source of truth).
  Also FILE_CASES and `-O2` smoke (hello.py + nbody.py at -O2).
- `exec()`/`eval()` intentionally unsupported. Real CPython stdlib modules not
  importable — synthetic ones only (`sys`, `re`, `os`, `subprocess`,
  `functools`, `cmath`, `time.perf_counter`, `math`, `json`, `random`,
  `itertools`, `collections`, `datetime`, `hashlib`, `base64`, `struct`,
  `heapq`, `bisect`, `statistics`, `string`, `textwrap`, `copy`, `uuid`,
  `operator`, `shutil`, `glob`, `csv`, `decimal`, `pathlib`).
- Past crash classes: "first star-prefixed name wins" bugs in `**kwargs`
  handling / adapter generation.
- No formal lint/typecheck/CI; the runner IS the verification.
- Trust the executable over the prose when they conflict.

## 6. Command cheat sheet for next session

```bash
# doc chunk
sed -n '1,300p' IMPLEMENTATION.md
# frontend coverage: find all parse entry points
grep -n '^\s*\(AstNode\|Node\|std::\).* parse[A-Z]' src/frontend/PythonParser.cpp
# open issues scan
grep -n -i 'open\|unresolved\|TODO' ISSUES.md | head -50
# test coverage scale
wc -l tests/runner.py
```
