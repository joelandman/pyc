# Next moves

## Immediate

- The location-slot commit is `adc0c4e`. Treat the CI `verify.yml` run as the
  authoritative gate for a push. A local language fast gate on this tree
  finished in 547s: 860/860 impactful, 858 byte-identical.
- Watch CI for:
  - language + known-gaps + concurrency gate at `881/881`
  - no new `DID_NOT_COMPILE`
  - no new silent-wrong answers
  - no `libpython` `DT_NEEDED` in the smoke binary

## Next correctness work

- Run the long `Lib/test` metric separately before changing any I6 numbers:

  ```bash
  export PYC_BINARY="$PWD/compiler/tools/pycc"
  export PYC_SYSROOT="$HOME/opt/py-sysroots/cp314-3.14.7-tier1"
  export PYC_LOWER=/tmp/pyc_lower
  make -C verify metric
  ```

- If `metric` changes the Lib/test result, update
  `compiler/baseline-libtest.json` and README/STATUS I6 numbers only with a
  measured reason. Do not hand-edit I6 numbers.
- Continue P0 `EXIT_DIFFERS` triage from `rebuild/STATUS.md`. Dump raw stderr,
  IR, and the narrow failing case before adding syntax or runtime features.
- Keep the new frame probe as the regression anchor:
  `verify/corpus/language/frame_lineno_and_traceback.py`.

## Process notes

- `compiler/baseline-language.json` currently records `jobs: 16` because it was
  refreshed locally. Job count is only a comparison note, not a gate condition,
  but future baseline refreshes should ideally come from a stable machine or CI
  if we want the record to be less local.
- Local wheel/NumPy smoke is not reliable here because the sysroot currently
  lacks `numpy`. Use the CI wheel step for that gate.
- The push check is `verify.yml`. It is green on the recent devel pushes.
  The red check on the branch is the scheduled `metric.yml` run, which does
  not start on push. Two follow-up pushes a few seconds apart used to cancel
  the first verify (`cancel-in-progress`); that cancellation is a red check
  on the commit that was superseded. The workflow now queues the second run,
  so the first commit's check finishes. Queued runs still serialize on
  `verify-<ref>`: wait for the in-progress verify before pushing again when
  you want that commit's result without a second runner sitting idle.
- A nightly Lib/test failure is a regression only when a case that passed
  in `compiler/baseline-libtest.json` fails against a stable oracle.
  `ORACLE_UNSTABLE` together with a stdout or exit difference has no
  ground truth and must not fail the gate. `test_xpickle.py` runs extra
  bodies on the runner because Ubuntu ships another Python; the metric
  job removes those versioned binaries so the nightly scores the same
  suite the baseline did.
- `pack-pyc.yml` must not list the archive
  with `tar | head` under `pipefail`. `metric.yml` runs
  `verify/longrunning.py` at `--longrunning-jobs 1` so `test_zipfile64`
  is not one of four concurrent 600s processes on the 4-core runner.
  Those files stay in the I6 denominator. A runner shutdown (exit 143)
  is not a compiler regression; the compare step never ran.
- Do not stage the existing untracked junk in the repo root (`.claude/`,
  `a.out_b7_modules.c`, `build.sh`, `junk95142.zip`, `prompt.txt`,
  `ziptestmodule`, `@test_*_tmp*`) with future commits.
