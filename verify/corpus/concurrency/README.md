# concurrency corpus

Threaded programs, compared against a real CPython the same way every other
corpus is (CHARTER I5). Two rules make that possible at all, and both are
constraints on the CASES rather than on the instrument:

**Every case must be deterministic in its output.** The comparison is
byte-for-byte, so nothing here may print an interleaving, a thread name, a
timing, or an identity. Print aggregates: a total, a sorted list, a count, a
boolean. A case whose output depends on scheduling is `ORACLE_UNSTABLE` and
measures nothing.

**No case may busy-wait.** Coordination goes through primitives that block and
release the GIL — `join`, `Event.wait`, `Queue.get`, `Barrier.wait`. A spin
loop hangs on the `cp314` target for a reason already recorded
(`known-gaps/thread_starvation.py`, defect E): a compiled loop never offers the
GIL. Spinning here would mean the whole corpus measured that one gap over and
over.

## What it is for

pyc manipulates refcounts directly from compiled code. Under `cp314t`
(free-threaded, PEP 703) those become atomic and biased, and the object layout
changes. This corpus exists to exercise that, because the language corpus is
almost entirely single-threaded and cannot: a clean `cp314t` run there shows the
ABI does not break the foundation, not that compiled code is thread-safe.

Run against both targets, which are separate under I8:

```bash
SR=~/opt/py-sysroots/cp314-3.14.7-tier1
FT=~/opt/py-sysroots/cp314t-3.14.7-tier1

./verify/measure_run.py --corpus verify/corpus/concurrency \
  --pyc "$PWD/compiler/tools/pycc" --pyc-flag=-O0 --sysroot "$SR"

PYC_SYSROOT="$FT" ./verify/measure_run.py --corpus verify/corpus/concurrency \
  --pyc "$PWD/compiler/tools/pycc" --pyc-flag=-O0 --oracle "$FT/bin/python3.14t"
```

A divergence that appears on one target and not the other is the interesting
result; it is not a regression in the other's baseline, because the oracle
differs and `measure_compare` refuses to compare across oracles.
