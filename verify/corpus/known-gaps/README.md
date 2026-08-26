# known-gaps — probes that currently FAIL

**Not wired into the test matrix.** Running these against the current compiler
produces P0 silent wrong answers and crashes. They live here so the gap is
written down and executable, not so CI goes red.

Run them deliberately:

```bash
S=~/opt/py-sysroots/cp314-3.14.7-tier1
./verify/measure_run.py --corpus verify/corpus/known-gaps \
  --pyc "$PWD/compiler/tools/pycc" --pyc-flag=-O0 --sysroot "$S" --jobs 4
```

These probes cover **three** defects, of which **one is now fixed**: nine for
missing Python frames (issue #9), nine `comp_cell_` for a raw cell leaking out
of a comprehension (**FIXED**, `fb54be2` — kept here as regression cover), and
three `nested_class_` for a broken closure chain through a class body. The last
two were found on 2026-08-26 by decomposing the 128 `EXIT_DIFFERS` cases in the
`Lib/test` baseline, and together they accounted for the two largest clusters
in it.

## A: no Python frame (issue #9)

Nine probes, one cause — compiled code runs with no Python frame
(issue #9, and `rebuild/CHARTER.md` "Deferred: per-function Python frames").

| probe | pyc | CPython |
|---|---|---|
| `locals_none_p0.py` | `None` | `{'a': 1}` |
| `globals_none_p0.py` | `globals() is None` → `True` | `False` |
| `locals_bool_p0.py` | `bool(locals())` → `False` | `True` |
| `vars_none_p0.py` | `SystemError: frame does not exist` | works |
| `eval_min.py`, `eval_in_func.py` | `TypeError: eval must be given globals and locals when called without a frame` | `2` |
| `exec_min.py` | `SystemError: globals and locals cannot be NULL` | works |
| `frame_builtins.py`, `globals_probe.py` | `AttributeError`/`TypeError` on `None` | work |

The first three are **silent**: exit status 0 with a wrong value. The rest fail
loudly. Same missing frame either way — only some paths raise.

`measure_run.py --fail-on-silent-wrong` reads `exit == 0` and `STDOUT_DIFFERS`
straight off the record (CHARTER I1), with no baseline involved.

## B: a raw cell leaks out of comprehensions — FIXED 2026-08-26 (`fb54be2`)

All nine now pass. They stay in this directory as regression cover: the defect
was invisible to a 747-case corpus, so the probes are the only thing standing
between it and a silent return.

A free variable read inside a **natively lowered comprehension** yielded the
`PyCell` itself rather than its contents.

```python
def outer():
    flag = False
    return [bool(flag) for i in range(2)]

# CPython -> [False, False]
# pyc     -> [True, True]      exit 0, no diagnostic
```

A cell is always truthy, so `bool()` returned `True` **unconditionally** — not
an inversion. The same compiled code was therefore right or wrong depending on
the runtime value, and silently right whenever the captured value was truthy.
`str()` and plain storage printed `<cell at 0x...: int object at 0x...>`. All
at exit 0 — **P0**.

**Cause:** `nested_reads()` treats a comprehension body as a nested scope, so
mentioning `n` in `[n * i for i in ...]` is exactly what gives `n` a cell slot.
`lower_comp` then passed captured locals as hidden arguments with a raw
`LoadLocal` on that slot, and `begin_function` cleared `cells_`, so the body
read the parameter as a plain local. The fix marks the hidden parameter as a
cell inside the synthetic function, so `lower_name` emits `cell.get` for it as
it does everywhere else. The cell is passed *through* rather than dereferenced,
so a rebind during iteration is still seen (I2).

| probe | shape | pyc |
|---|---|---|
| `comp_cell_bool_p0.py` | `bool(free)` | `[True, True]` for `[False, False]` — **silent** |
| `comp_cell_value_p0.py` | `[free for ...]` | the cell object — **silent** |
| `comp_cell_str_p0.py` | `str(free)` | the cell's repr — **silent** |
| `comp_cell_nested.py` | comp inside comp | the cell object — **silent** |
| `comp_cell_arith.py` | `free * i` | `TypeError: ... 'cell' and 'int'` — loud |
| `comp_cell_param.py` | free var is a *parameter* | same TypeError — loud |
| `comp_cell_condition.py` | `if i > free` | `TypeError: '>' not supported` — loud |
| `comp_cell_dict_set.py` | dict and set comprehensions | same — loud |
| `comp_cell_genexp_ok.py` | **control**: generator expression | **correct** |

The control is the point. A genexp reading the same free variable is right,
because genexps are handed to CPython as marshalled code objects rather than
lowered natively (`compiler/pyc_parse/genexp.py`). That contrast localises the
defect to the native comprehension path. Module globals are also unaffected —
only cell reads.

**The corpus could not see this.** An AST walk over all 747 `language/` cases
found **zero** with a comprehension reading an enclosing function local, which
is why the rate sat at 98.80% with a P0 live in one of Python's most common
idioms. Worth remembering the next time a high pass rate feels like coverage.

**pyc is not converting NULL to None.** Calling these builtins directly from a
frameless embedded interpreter returns `Py_None` from CPython's own C code, so
the fault is the absent frame, not a missing NULL check at the call site. That
was checked, because the opposite conclusion is the natural one to reach and
would send someone hunting in the wrong file.

When frames land, these should move into `language/` and the gate will hold
them.


## C: the closure chain breaks through a class body (found 2026-08-26)

A method of a class defined **inside a function** cannot see that function's
locals.

```python
def f():
    output = []
    class T:
        def go(self):
            output.append("called")
    T().go()
    return output

# CPython -> ['called']
# pyc     -> NameError: name 'output' is not defined
```

| probe | shape | pyc |
|---|---|---|
| `nested_class_closure.py` | method reads an enclosing function local | `NameError: name 'output'` |
| `nested_class_selfref.py` | method refers to its own class by name | `NameError: name 'T'` |
| `nested_class_body_ok.py` | **control**: class *body* reads the local | **correct** |

The control narrows it: class scoping in general is fine. Only a method — two
scopes down, through the class body — loses the link.

`nested_class_selfref` is worth reading twice. The class's own name is a local
of the enclosing function, so a method constructing another instance of its own
class is doing a skip-level free-variable read. That is an ordinary idiom, and
it is the exact shape behind `test_abstract_numbers`' `MyComplex` and
`test_bool`'s `SymbolicBool`.

This defect is **loud** — always a `NameError`, never a wrong answer — so it is
I1-clean. It is simply very common: 23 of the 128 `EXIT_DIFFERS` files in
`Lib/test` fail this way, because defining a helper class inside a test method
and closing over the test's locals is how unittest suites are written.
