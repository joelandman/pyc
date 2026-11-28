# stress — unmapped-construct probes

A focused probe set for constructs that the gated `language/` corpus (728
cases) does not reach in depth: advanced stdlib (`dataclasses`, `functools`,
`collections`, `enum`, `abc`, `contextlib`), advanced syntax/semantics
(`match` class+guard patterns, descriptors, `@dataclass`/`@property`/class
decorators, metaclasses, `super()`/MRO, `yield from`, `except*`), and common
stdlib modules (`re`, `json`, `decimal`, `fractions`, `itertools`).

**Wired into the CI as the coverage boundary** (the `coverage` job in
`.github/workflows/verify.yml`): the on-disk pass rate is gated on
`--fail-on-silent-wrong`, which fails the job only on a silent-wrong answer
(CHARTER I1). It is not a monotonicity gate — it needs no baseline, because a
silent-wrong answer is a property of this run alone. A clean pass, an honest
refusal (`DID_NOT_COMPILE` — the acceptable outcome for an unsupported
construct, I1a), or an oracle-unstable/DIFF case all pass; only a silent
wrong answer fails. Run it locally the same way:

```bash
S=~/opt/py-sysroots/cp314-3.14.7-tier1
./verify/measure_run.py --corpus verify/corpus/stress \
  --pyc "$PWD/compiler/tools/pyc" --pyc-flag=-O0 --sysroot "$S" --json stress.json
```

`--fail-on-silent-wrong` is the important flag here: it exits 1 if any case
exits 0 while printing something other than CPython's answer (CHARTER I1 — a
silent wrong answer). That is the failure mode a stress survey exists to catch;
a `DID_NOT_COMPILE` (a refusal that names the construct) is the *acceptable*
outcome for an unsupported construct, and is reported separately in the record.

Results are triaged into three buckets:

- **PASS** — byte-identical to CPython. The construct is supported.
- **REFUSED** — `DID_NOT_COMPILE` whose diagnostic names the construct, line,
  and reason (CHARTER I1). Acceptable; it is the coverage boundary, honestly
  marked.
- **SILENT WRONG / DIFF** — compiles but diverges. Highest priority; this is
  an I1 violation and is reported above everything else.

A probe that PASSES and is idiomatic enough to belong permanently may be moved
into `language/` (gated) once confirmed. A probe that REFUSES cleanly may be
moved into `known-gaps/` (baselined) once confirmed — though `known-gaps/`
currently reports no open gaps, so a genuinely-new refusal is new information,
not a re-discovery.

## Boundary (intentional refusals)

`reject/` holds probes that are *valid CPython* but that `pyc` deliberately
refuses. They are not globbed by `--corpus verify/corpus/stress` (non-recursive
`*.py`), so they do not affect that pass rate; they document where the coverage
boundary is. Run them on their own:

```bash
./verify/measure_run.py --corpus verify/corpus/stress/reject \
  --pyc "$PWD/compiler/tools/pyc" --pyc-flag=-O0 --sysroot "$S"
```

`reject/d5b_match_as_pattern.py`: `case int() as v` is valid CPython but
`pyc`'s match parser rejects it at the `as` token with "invalid syntax" — the
as-binding-on-a-class-pattern form is the boundary the parser has not reached.
