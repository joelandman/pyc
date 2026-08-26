# boundary — numeric and Unicode edge probes

**In the repo, not wired into the test matrix.** All 27 currently pass.

They are kept because they are useful to run by hand after touching value
handling, and because a regression here would be serious. They are not in the
gate for an honest reason: pyc links libpython and delegates int/float/str/bytes
semantics to the same C code CPython runs, so most of these match **by
construction** rather than by pyc doing anything right. Wiring them in would add
runtime to the gate and buy little signal.

The probes that genuinely exercise pyc's own frontend — literals, escapes,
f-strings, constant folding, source encodings, NFKC identifier normalization,
embedded NUL — were promoted into `language/` and *are* gated.

```bash
S=~/opt/py-sysroots/cp314-3.14.7-tier1
./verify/run.py --corpus verify/corpus/boundary \
  --pyc "$PWD/compiler/tools/pycc" --pyc-flag -O0 --sysroot "$S" --jobs 4
```

Coverage: float precision and 17-digit repr, DBL_MIN/DBL_MAX, 5e-324, inf/nan/
signed zero, copysign, format specs, fmod vs `%`, fsum; 2**53 boundaries,
banker's rounding, exact int/float comparison, arbitrary-precision ints,
`int_max_str_digits`, int64→bigint promotion; negative floor-division and
modulo (where Python differs from C), infinite two's-complement bit ops,
ZeroDivisionError/OverflowError message text; code-point `len`, astral plane and
U+10FFFF, utf-8/16/32/latin-1 round trips with every error handler, lone
surrogates, case mapping beyond ASCII, NFC/NFD/NFKC/NFKD.

**Determinism matters more than coverage here.** Three probes had to be reworked
to be byte-stable: a `SyntaxWarning` that carries the source path (which the
harness copies to a fresh temp dir, so it differs per side), an unguarded
`OverflowError` that aborted the rest of a probe, and a `1 << 2**40` shift that
is a ~128 GB allocation and hence a MemoryError-or-hang coin flip. A probe whose
output varies between two runs of the same binary is worse than no probe.
