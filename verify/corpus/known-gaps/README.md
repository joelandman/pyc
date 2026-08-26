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

All nine trace to **one** cause — compiled code runs with no Python frame
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

`measure_run.py --fail-on-silent-wrong` picks out exactly those three from this
directory and nothing else, with no baseline involved: it reads `exit == 0` and
`STDOUT_DIFFERS` off the record (CHARTER I1).

**pyc is not converting NULL to None.** Calling these builtins directly from a
frameless embedded interpreter returns `Py_None` from CPython's own C code, so
the fault is the absent frame, not a missing NULL check at the call site. That
was checked, because the opposite conclusion is the natural one to reach and
would send someone hunting in the wrong file.

When frames land, these should move into `language/` and the gate will hold
them.
