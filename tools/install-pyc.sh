#!/usr/bin/env bash
#
# install-pyc.sh — copy a relocatable pyc prefix (S5).
#
#   ./tools/install-pyc.sh --prefix DIR
#   ./tools/install-pyc.sh --prefix DIR --no-sysroot
#   ./tools/install-pyc.sh --prefix DIR --copy-sysroot
#   ./tools/install-pyc.sh --prefix DIR --from-archive FILE.tar.xz
#
# Layout:
#   DIR/bin/pycc
#   DIR/bin/pyc_lower
#   DIR/lib/pyc/{pyc_parse,include,src/rt,install-sysroot.sh,pyc-prefix.json}
#   DIR/sysroots/cp314-3.14.7-tier1/   (optional)
#
# After this, DIR/bin/pycc works with no PYC_SYSROOT / PYC_LOWER / repo cwd.
# Linking a user program still needs clang++/LLVM 22 on PATH (or PYC_CXX).
#
set -euo pipefail

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX=""
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
NO_SYSROOT=0
COPY_SYSROOT=0
SKIP_BUILD=0
ARCHIVE=""

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?}"; shift 2;;
    --jobs|-j) JOBS="${2:?}"; shift 2;;
    --no-sysroot) NO_SYSROOT=1; shift;;
    --copy-sysroot) COPY_SYSROOT=1; shift;;
    --skip-build) SKIP_BUILD=1; shift;;
    --from-archive) ARCHIVE="${2:?}"; shift 2;;
    -h|--help) usage;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

[[ -n "$PREFIX" ]] || die "--prefix DIR is required"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

if [[ -n "$ARCHIVE" ]]; then
  [[ -f "$ARCHIVE" ]] || die "no archive $ARCHIVE"
  info "unpack $ARCHIVE -> $PREFIX"
  tar -C "$PREFIX" -xJf "$ARCHIVE"
  [[ -x "$PREFIX/bin/pycc" ]] || die "archive has no bin/pycc"
  info "installed"
  echo "  $PREFIX/bin/pycc"
  echo "  export PATH=$PREFIX/bin:\$PATH"
  exit 0
fi

pick_cxx() {
  local c
  for c in "${PYC_CXX:-}" clang++-22 clang++; do
    [[ -n "$c" ]] || continue
    command -v "$c" >/dev/null 2>&1 && { printf '%s' "$c"; return 0; }
  done
  return 1
}

CXX="$(pick_cxx || true)"
[[ -n "$CXX" ]] || die "no clang++ / clang++-22 (LLVM 22)"

info "compiler $CXX"
mkdir -p "$PREFIX/bin" "$PREFIX/lib/pyc/src" "$PREFIX/sysroots"
if (( ! SKIP_BUILD )); then
  make -C "$REPO/compiler" CXX="$CXX" OUT="$PREFIX/bin/pyc_lower"
else
  [[ -x "$PREFIX/bin/pyc_lower" ]] || die "--skip-build but no $PREFIX/bin/pyc_lower"
fi

info "layout $PREFIX"
cp -f "$REPO/compiler/tools/pycc" "$PREFIX/bin/pycc"
chmod +x "$PREFIX/bin/pycc" "$PREFIX/bin/pyc_lower"
rm -rf "$PREFIX/lib/pyc/pyc_parse" "$PREFIX/lib/pyc/include" "$PREFIX/lib/pyc/src/rt"
cp -a "$REPO/compiler/pyc_parse" "$PREFIX/lib/pyc/pyc_parse"
cp -a "$REPO/compiler/include" "$PREFIX/lib/pyc/include"
cp -a "$REPO/compiler/src/rt" "$PREFIX/lib/pyc/src/rt"
cp -f "$REPO/tools/install-sysroot.sh" "$PREFIX/lib/pyc/install-sysroot.sh"
chmod +x "$PREFIX/lib/pyc/install-sysroot.sh"
find "$PREFIX/lib/pyc" -type d -name '__pycache__' -exec rm -rf {} +
find "$PREFIX/lib/pyc" -name '*.pyc' -delete

git_rev="$(git -C "$REPO" describe --always --dirty --tags 2>/dev/null || echo unknown)"
python3 - "$PREFIX/lib/pyc/pyc-prefix.json" "$git_rev" "$(uname -m)" "$(uname -s)" <<'PY'
import json, os, sys
path, git, machine, osname = sys.argv[1:5]
data = {
    "kind": "pyc-prefix",
    "git": git,
    "machine": machine,
    "os": osname.lower(),
}
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, "w") as f:
    json.dump(data, f, indent=2, sort_keys=True)
    f.write("\n")
PY

SR_NAME=cp314-3.14.7-tier1
DEST="$PREFIX/sysroots/$SR_NAME"
HOME_SR="${PYC_SYSROOT:-$HOME/opt/py-sysroots/$SR_NAME}"
if (( NO_SYSROOT )); then
  info "no sysroot in prefix; $PREFIX/bin/pycc --fetch-sysroot after install"
elif [[ -x "$HOME_SR/bin/python3.14" || -x "$HOME_SR/bin/python3" ]]; then
  if (( COPY_SYSROOT )); then
    info "copying $HOME_SR -> $DEST"
    rm -rf "$DEST"
    mkdir -p "$DEST"
    tar -C "$HOME_SR" -cf - . | tar -C "$DEST" -xf -
  else
    info "linking $DEST -> $HOME_SR"
    ln -sfn "$HOME_SR" "$DEST"
  fi
elif [[ -x "$REPO/compiler/tools/sysroot/bin/python3.14" ]]; then
  info "linking $DEST -> $REPO/compiler/tools/sysroot"
  ln -sfn "$REPO/compiler/tools/sysroot" "$DEST"
else
  info "no local sysroot; $PREFIX/bin/pycc --fetch-sysroot after install"
fi

info "installed"
echo "  $PREFIX/bin/pycc"
echo "  export PATH=$PREFIX/bin:\$PATH"
