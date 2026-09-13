#!/usr/bin/env bash
#
# install-pyc.sh — copy a relocatable pyc prefix (S5).
#
#   ./tools/install-pyc.sh --prefix DIR
#
# Layout:
#   DIR/bin/pycc
#   DIR/bin/pyc_lower
#   DIR/lib/pyc/{pyc_parse,include,src/rt,install-sysroot.sh}
#   DIR/sysroots/cp314-3.14.7-tier1/   (symlink or unpack)
#
# A second Python (e.g. 3.13) is another directory under sysroots/, same
# shape, selected with --python=X.Y. One published 3.14 tree is enough.
#
# After this, DIR/bin/pycc works with no PYC_SYSROOT / PYC_LOWER / repo cwd.
#
set -euo pipefail

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX=""
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?}"; shift 2;;
    --jobs|-j) JOBS="${2:?}"; shift 2;;
    -h|--help) usage;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

[[ -n "$PREFIX" ]] || die "--prefix DIR is required"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

CXX=clang++
command -v clang++-22 >/dev/null 2>&1 && CXX=clang++-22

info "compiler"
make -C "$REPO/compiler" CXX="$CXX"

info "layout $PREFIX"
mkdir -p "$PREFIX/bin" "$PREFIX/lib/pyc/src" "$PREFIX/sysroots"
cp -f "$REPO/compiler/tools/pycc" "$PREFIX/bin/pycc"
chmod +x "$PREFIX/bin/pycc"
if [[ -x /tmp/pyc_lower ]]; then
  cp -f /tmp/pyc_lower "$PREFIX/bin/pyc_lower"
elif [[ -x "$REPO/compiler/tools/pyc_lower" ]]; then
  cp -f "$REPO/compiler/tools/pyc_lower" "$PREFIX/bin/pyc_lower"
else
  die "no pyc_lower (make -C compiler)"
fi
chmod +x "$PREFIX/bin/pyc_lower"
rm -rf "$PREFIX/lib/pyc/pyc_parse" "$PREFIX/lib/pyc/include" "$PREFIX/lib/pyc/src/rt"
cp -a "$REPO/compiler/pyc_parse" "$PREFIX/lib/pyc/pyc_parse"
cp -a "$REPO/compiler/include" "$PREFIX/lib/pyc/include"
cp -a "$REPO/compiler/src/rt" "$PREFIX/lib/pyc/src/rt"
cp -f "$REPO/tools/install-sysroot.sh" "$PREFIX/lib/pyc/install-sysroot.sh"
chmod +x "$PREFIX/lib/pyc/install-sysroot.sh"

SR_NAME=cp314-3.14.7-tier1
DEST="$PREFIX/sysroots/$SR_NAME"
HOME_SR="${PYC_SYSROOT:-$HOME/opt/py-sysroots/$SR_NAME}"
if [[ -x "$HOME_SR/bin/python3.14" || -x "$HOME_SR/bin/python3" ]]; then
  info "linking $DEST -> $HOME_SR"
  ln -sfn "$HOME_SR" "$DEST"
elif [[ -x "$REPO/compiler/tools/sysroot/bin/python3.14" ]]; then
  info "linking $DEST -> $REPO/compiler/tools/sysroot"
  ln -sfn "$REPO/compiler/tools/sysroot" "$DEST"
else
  info "no local sysroot; $PREFIX/bin/pycc --fetch-sysroot after install"
fi

info "installed"
echo "  $PREFIX/bin/pycc"
echo "  export PATH=$PREFIX/bin:\$PATH"
