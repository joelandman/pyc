#!/usr/bin/env bash
#
# pack-pyc.sh — tar a relocatable pyc prefix (compiler, not CPython).
#
# Two published artifacts:
#   pyc-linux-x86_64.tar.xz          this script (pycc, pyc_lower, lib/pyc)
#   cp314-*-tier1-x86_64.tar.xz      pack-sysroot.sh (already)
#
# The compiler tarball does not include the sysroot. After unpack:
#   tar -C "$PREFIX" -xJf pyc-linux-x86_64.tar.xz
#   export PATH="$PREFIX/bin:$PATH"
#   pycc --fetch-sysroot
#   pycc hello.py -o hello
#
# Host still needs clang++/LLVM 22 and ld.lld to *link* user programs.
#
#   ./tools/pack-pyc.sh -o /tmp/pyc-linux-x86_64.tar.xz
#   ./tools/pack-pyc.sh --with-sysroot -o /tmp/pyc-fat.tar.xz
#   ./tools/pack-pyc.sh --dry-run
#
set -euo pipefail

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=""
DRY_RUN=0
WITH_SYSROOT=0
STAGE=""

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -o|--output) OUT="${2:?}"; shift 2;;
    --with-sysroot) WITH_SYSROOT=1; shift;;
    --stage) STAGE="${2:?}"; shift 2;;
    --dry-run) DRY_RUN=1; shift;;
    -h|--help) usage;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

machine="$(uname -m)"
os="$(uname -s | tr '[:upper:]' '[:lower:]')"
[[ -n "$OUT" ]] || OUT="pyc-${os}-${machine}.tar.xz"
OUT="$(mkdir -p "$(dirname "$OUT")" && cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"

if (( DRY_RUN )); then
  printf '  would: install-pyc.sh --prefix STAGE%s && tar -C STAGE -cJf %q bin lib sysroots\n' \
    "$( (( WITH_SYSROOT )) && echo ' --copy-sysroot' || echo ' --no-sysroot' )" "$OUT"
  exit 0
fi

if [[ -z "$STAGE" ]]; then
  STAGE="$(mktemp -d "${TMPDIR:-/tmp}/pyc-prefix.XXXXXX")"
  trap 'rm -rf "$STAGE"' EXIT
fi
mkdir -p "$STAGE"
STAGE="$(cd "$STAGE" && pwd)"

inst=("$REPO/tools/install-pyc.sh" --prefix "$STAGE")
if (( WITH_SYSROOT )); then
  inst+=(--copy-sysroot)
else
  inst+=(--no-sysroot)
fi
"${inst[@]}"

info "archive $OUT"
mkdir -p "$(dirname "$OUT")"
tar -C "$STAGE" -cJf "$OUT" \
  --exclude='__pycache__' \
  --exclude='*.pyc' \
  bin lib sysroots
info "wrote $OUT ($(wc -c < "$OUT") bytes)"
