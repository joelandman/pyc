#!/usr/bin/env bash
#
# check-nightly.sh — did the nightly metric cron run, and what did it say?
#
# The `metric` workflow fires at 06:00 UTC. Its schedule trigger works only
# from the DEFAULT branch, which is why `devel` is the default -- while `main`
# held that role the workflow was invisible to GitHub and no cron ever ran.
# So the first question is always "did a scheduled run happen at all", not
# "did it pass".
#
#   ./tools/check-nightly.sh              # latest scheduled run
#   ./tools/check-nightly.sh --any        # latest run of any trigger
#   ./tools/check-nightly.sh --log        # also dump the verdict block
#
# Exits 0 if the run passed, 1 if it failed or none was found.
set -uo pipefail

REPO="${PYC_REPO:-joelandman/pyc}"
EVENT="schedule"
SHOW_LOG=0
for a in "$@"; do
  case "$a" in
    --any) EVENT="" ;;
    --log) SHOW_LOG=1 ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

command -v gh >/dev/null || { echo "gh not installed" >&2; exit 2; }

q=(--workflow metric.yml --limit 10 --json databaseId,event,status,conclusion,createdAt,url)
mapfile -t rows < <(gh run list -R "$REPO" "${q[@]}" \
  --jq ".[] | select(\"$EVENT\" == \"\" or .event == \"$EVENT\") |
        \"\(.databaseId)\t\(.event)\t\(.status)\t\(.conclusion // \"-\")\t\(.createdAt)\t\(.url)\"" 2>/dev/null)

if [ "${#rows[@]}" -eq 0 ]; then
  if [ -n "$EVENT" ]; then
    echo "NO SCHEDULED RUN FOUND."
    echo
    echo "  That is itself the finding: the 06:00 UTC cron did not fire."
    echo "  Check that 'devel' is still the default branch -- schedule: and"
    echo "  workflow_dispatch: only run from it. Re-run with --any to see"
    echo "  whether manual runs are happening."
  else
    echo "NO RUNS FOUND for metric.yml in $REPO."
  fi
  exit 1
fi

IFS=$'\t' read -r id event status conclusion created url <<<"${rows[0]}"
echo "run        $id"
echo "trigger    $event"
echo "created    $created"
echo "status     $status  ${conclusion}"
echo "url        $url"
echo

if [ "$status" != "completed" ]; then
  echo "Still running — re-check shortly."
  exit 1
fi

log="$(gh run view "$id" -R "$REPO" --log 2>/dev/null)"

# The verdict banner is the point: GitHub renders every nonzero exit as the
# same red X, so a "gate did not run" (nothing verified) and a real regression
# are indistinguishable from the run list alone.
verdict="$(grep -oE 'GATE RAN — no regression|REGRESSION — the gate ran and FAILED|GATE DID NOT RUN — [a-z ]+|HARNESS USAGE ERROR|UNKNOWN GATE STATE' <<<"$log" | head -1)"
echo "verdict    ${verdict:-<none found — the Verdict step may not have run>}"

rate="$(grep -oE '### \*\*[0-9.]+%\*\*  \([0-9]+/[0-9]+ files\)' <<<"$log" | head -1 \
        | sed 's/### \*\*//; s/\*\*//')"
if [ -z "$rate" ]; then
  # Runs before the Publish step teed to stdout have the number only in their
  # step summary, which the log does not carry. The artifact is authoritative.
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  if gh run download "$id" -R "$REPO" -n libtest-metric -D "$tmp" >/dev/null 2>&1; then
    rate="$(python3 -c "
import json,glob,sys
f=glob.glob('$tmp/*.json')
if not f: sys.exit(1)
d=json.load(open(f[0]))
t=d.get('total', len(d.get('results', [])))
print(f\"{d['pass_rate']:.2f}%  ({d['passing']}/{t} files)  [from artifact]\")" 2>/dev/null)"
  fi
fi
[ -n "$rate" ] && echo "published  $rate"

grep -q 'BASELINE IS BEHIND' <<<"$log" && \
  echo "note       BASELINE IS BEHIND — this run beat its baseline; refresh it"

if grep -q 'Cache restored from key: sysroot' <<<"$log"; then
  echo "sysroot    cache hit"
else
  echo "sysroot    cache MISS (rebuilt; ~20 min of PGO)"
fi

(( SHOW_LOG )) && { echo; sed -n '/Verdict/,/Publish the number/p' <<<"$log" | head -30; }

echo
case "$verdict" in
  "GATE RAN"*)          echo "OK — the gate ran and nothing regressed."; exit 0 ;;
  "REGRESSION"*)        echo "REAL FINDING — something the compiler does got worse."; exit 1 ;;
  "GATE DID NOT RUN"*)  echo "NOTHING WAS VERIFIED — not a regression. The metric is unguarded."; exit 1 ;;
  *)                    [ "$conclusion" = success ] && exit 0 || exit 1 ;;
esac
