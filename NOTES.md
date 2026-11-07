# Next moves

## Immediate

- 2026-11-07 state: HEAD is `db2b40a` (pre-MVP blocker pass). Local gates on
  this tree: `make -C verify fast` 865/865 impactful; `make -C verify verify`
  887/887, gate "OK — nothing regressed". I6 baseline is 376/389 (96.66%).
- Treat the CI `verify.yml` run as the authoritative gate for a push.
  Watch CI for:
  - language + known-gaps + concurrency gate at `887/887`
  - no new `DID_NOT_COMPILE`
  - no new silent-wrong answers
  - no `libpython` `DT_NEEDED` in the smoke binary
  - the new `check_diagnostics` static-check step (M6 contract)

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
  measured reason. Do not hand-edit I6 numbers. Local re-records must also
  match CI's interpreter set: run under a PATH with versioned pythons hidden
  (a uv-managed 3.11 on this box made `test_xpickle` diverge on 2026-11-07 —
  see the STATUS 2026-11-07 entry (9)).
- The P0 `EXIT_DIFFERS` triage is closed: the two remaining `EXIT_DIFFERS`
  files are root-caused and decided (2026-11-07, static-libpython inspection
  deltas). Dump raw stderr, IR, and the narrow failing case before adding
  syntax or runtime features — that remains the rule for any NEW difference.
- Keep the new frame probe as the regression anchor:
  `verify/corpus/language/frame_lineno_and_traceback.py`.

## Process notes

- `compiler/baseline-language.json` records `jobs: 32` (2026-11-07 local
  refresh). Job count is only a comparison note, not a gate condition, but
  future baseline refreshes should ideally come from a stable machine or CI
  if we want the record to be less local.
- Local wheel/NumPy smoke now works: a numpy-carrying copy of the sysroot
  lives at `$HOME/opt/py-sysroots/cp314-3.14.7-tier1-numpy` (recipe in
  README.md). The primary sysroot stays pristine; CI's nightly wheel step
  remains the per-commit wheel gate. Run the copy's python with
  `LD_LIBRARY_PATH` scrubbed (this shell's 3.14.5 libpython outranks the
  sysroot's RUNPATH).
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
