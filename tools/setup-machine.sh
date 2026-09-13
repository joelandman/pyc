#!/usr/bin/env bash
#
# setup-machine.sh — onboard a clone on a new machine (S2).
#
# Order: check host tools → fetch sysroot tarball (or build it) → unpack →
# build pyc_lower → smoke. Does not install LLVM; does not sudo.
#
#   ./tools/setup-machine.sh
#   ./tools/setup-machine.sh --beside
#   ./tools/setup-machine.sh --build-sysroot
#   ./tools/setup-machine.sh --github-repo joelandman/pyc
#
set -euo pipefail

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PYC_SYSROOT:-$HOME/opt/py-sysroots/cp314-3.14.7-tier1}"
GH_REPO="joelandman/pyc"
TAG="sysroot-cp314-linux-x86_64"
ASSET="cp314-3.14.7-tier1-x86_64.tar.xz"
BUILD_SYSROOT=0
SKIP_COMPILER=0
BESIDE=0
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?}"; shift 2;;
    --github-repo) GH_REPO="${2:?}"; shift 2;;
    --tag) TAG="${2:?}"; shift 2;;
    --asset) ASSET="${2:?}"; shift 2;;
    --beside) BESIDE=1; shift;;
    --build-sysroot) BUILD_SYSROOT=1; shift;;
    --skip-compiler) SKIP_COMPILER=1; shift;;
    --jobs|-j) JOBS="${2:?}"; shift 2;;
    -h|--help) usage;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

cd "$REPO"
if (( BESIDE )); then
  PREFIX="$REPO/compiler/tools/sysroot"
fi

pick_cxx() {
  local c
  for c in clang++-22 clang++; do
    if command -v "$c" >/dev/null 2>&1; then
      printf '%s' "$c"
      return 0
    fi
  done
  return 1
}

step_host() {
  info "1/4 host tools"
  local missing=()
  command -v make >/dev/null 2>&1 || missing+=("make")
  command -v curl >/dev/null 2>&1 || missing+=("curl")
  command -v tar >/dev/null 2>&1 || missing+=("tar")
  command -v xz >/dev/null 2>&1 || missing+=("xz")
  CXX="$(pick_cxx || true)"
  [[ -n "${CXX:-}" ]] || missing+=("clang++-22 (LLVM 22)")
  (( ${#missing[@]} == 0 )) || die "missing: ${missing[*]}
  LLVM 22: https://apt.llvm.org (clang++-22, lld). CPython build deps only if you pass --build-sysroot."
  info "cxx $CXX"
}

fetch_archive() {
  local dest="$1"
  local url="https://github.com/${GH_REPO}/releases/download/${TAG}/${ASSET}"
  info "downloading $url"
  if ! curl -fL --retry 3 -o "$dest" "$url"; then
    rm -f "$dest"
    return 1
  fi
  return 0
}

step_sysroot() {
  info "2/4 sysroot"
  if [[ -x "$PREFIX/bin/python3.14" || -x "$PREFIX/bin/python3" ]]; then
    info "already present $PREFIX"
    "$REPO/tools/install-sysroot.sh" --prefix "$PREFIX" --verify-only
    return 0
  fi
  local home_default="$HOME/opt/py-sysroots/cp314-3.14.7-tier1"
  if (( BESIDE )) && [[ -x "$home_default/bin/python3.14" || -x "$home_default/bin/python3" ]]; then
    info "linking $PREFIX -> $home_default"
    mkdir -p "$(dirname "$PREFIX")"
    ln -sfn "$home_default" "$PREFIX"
    "$REPO/tools/install-sysroot.sh" --prefix "$PREFIX" --verify-only
    return 0
  fi
  if (( BUILD_SYSROOT )); then
    "$REPO/tools/build-python-sysroot.sh" --version 3.14.7 --jobs "$JOBS" --prefix "$PREFIX"
    return 0
  fi
  local archive="$REPO/$ASSET"
  if [[ ! -f "$archive" ]]; then
    fetch_archive "$archive" || die "no release asset at github.com/${GH_REPO}/releases/tag/${TAG}
  Wait for the sysroot workflow, pass --asset PATH, or rerun with --build-sysroot."
  fi
  "$REPO/tools/install-sysroot.sh" --archive "$archive" --prefix "$PREFIX"
}

step_compiler() {
  info "3/4 compiler"
  if (( SKIP_COMPILER )); then
    info "skipped"
    return 0
  fi
  make -C "$REPO/compiler" CXX="$CXX"
  if (( BESIDE )); then
    cp -f /tmp/pyc_lower "$REPO/compiler/tools/pyc_lower"
    chmod +x "$REPO/compiler/tools/pyc_lower"
    info "copied pyc_lower next to pycc"
  fi
}

step_smoke() {
  info "4/4 smoke"
  if (( SKIP_COMPILER )); then
    info "skipped"
    return 0
  fi
  local lower=/tmp/pyc_lower
  (( BESIDE )) && lower="$REPO/compiler/tools/pyc_lower"
  [[ -x "$lower" ]] || die "pyc_lower missing at $lower"
  local src="/tmp/pyc-setup-machine-smoke.py"
  printf 'print(2 ** 10)\n' > "$src"
  if (( BESIDE )); then
    env -u PYC_SYSROOT -u PYC_LOWER \
      "$REPO/compiler/tools/pycc" "$src" -o /tmp/pyc-setup-smoke -O0
  else
    PYC_SYSROOT="$PREFIX" PYC_LOWER=/tmp/pyc_lower \
      "$REPO/compiler/tools/pycc" "$src" -o /tmp/pyc-setup-smoke -O0
  fi
  local got
  got="$(PYTHONHOME="$PREFIX" /tmp/pyc-setup-smoke)"
  rm -f "$src" /tmp/pyc-setup-smoke
  [[ "$got" == "1024" ]] || die "smoke got '$got' want 1024"
  info "smoke 1024"
}

step_host
step_sysroot
step_compiler
step_smoke

if (( BESIDE )); then
  cat <<EOF

Ready. Sysroot and pyc_lower sit next to pycc; no env needed.
  $REPO/compiler/tools/pycc prog.py -o prog -O0
  # override: PYC_SYSROOT=... PYC_LOWER=...
EOF
else
  cat <<EOF

Ready.
  export PYC_SYSROOT=$PREFIX
  export PYC_LOWER=/tmp/pyc_lower
  $REPO/compiler/tools/pycc prog.py -o prog -O0
EOF
fi
