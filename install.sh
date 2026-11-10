#!/usr/bin/env bash
#
# install.sh — install pyc (compiler + CPython sysroot) into one prefix.
#
#   ./install.sh                            # default: $HOME/opt/pyc
#   ./install.sh --prefix /opt/pyc
#   ./install.sh --prefix DIR --dryrun      # print the plan, change nothing
#   ./install.sh --uninstall --prefix DIR   # remove the pyc install from DIR
#
# The prefix holds both trees; afterwards it needs neither this checkout
# nor any environment variable:
#
#   DIR/bin/pyc                       the compiler
#   DIR/lib/pyc/                      parse frontend + runtime sources
#   DIR/sysroots/cp314-3.14.7-tier1/  the CPython sysroot (the target)
#
# install  = host check -> compiler (tools/install-pyc.sh) -> sysroot
#            (release download by default; --build-sysroot builds CPython;
#            --sysroot DIR links an existing one, --copy-sysroot copies)
#            -> differential smoke: a program is compiled with DIR/bin/pyc
#            with NO PYC_SYSROOT (the prefix's own sysroots/ must resolve),
#            and its output is compared against the sysroot's own CPython
#            (I5: subject and oracle are the same CPython).
#
# uninstall removes only the paths pyc installed (bin/pyc, lib/pyc,
# sysroots/<name>): a symlinked sysroot removes the link, not its target,
# and the prefix itself and anything it does not own are never touched.
# Refuses without the lib/pyc/pyc-prefix.json marker unless --force.
#
# Env: PYC_CXX (the linking compiler; default clang++-22|clang++),
#      PYC_GITHUB_REPO / PYC_SYSROOT_TAG / PYC_SYSROOT_ASSET (mirror).
#
set -euo pipefail

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GH_REPO="${PYC_GITHUB_REPO:-joelandman/pyc}"
TAG="${PYC_SYSROOT_TAG:-sysroot-cp314-linux-x86_64}"
ASSET="${PYC_SYSROOT_ASSET:-cp314-3.14.7-tier1-x86_64.tar.xz}"
URL="https://github.com/${GH_REPO}/releases/download/${TAG}/${ASSET}"

PREFIX="$HOME/opt/pyc"
PREFIX_GIVEN=0
DRYRUN=0
UNINSTALL=0
BUILD_SYSROOT=0
COPY_SYSROOT=0
NO_SMOKE=0
FORCE=0
SRC_SYSROOT=""

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?}"; PREFIX_GIVEN=1; shift 2;;
    --dryrun) DRYRUN=1; shift;;
    --uninstall) UNINSTALL=1; shift;;
    --build-sysroot) BUILD_SYSROOT=1; shift;;
    --sysroot) SRC_SYSROOT="${2:?}"; shift 2;;
    --copy-sysroot) COPY_SYSROOT=1; shift;;
    --no-smoke) NO_SMOKE=1; shift;;
    --force) FORCE=1; shift;;
    -h|--help) usage;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

# The sysroot directory name inside the prefix: the asset minus its
# -<arch>.tar.xz suffix (cp314-3.14.7-tier1-x86_64.tar.xz -> cp314-3.14.7-tier1).
SR_NAME="${ASSET%.tar.xz}"
_arch_suffix="-$(uname -m)"
if [[ "$SR_NAME" == *$_arch_suffix ]]; then
  SR_NAME="${SR_NAME%"$_arch_suffix"}"
fi
if [[ -n "$SRC_SYSROOT" ]]; then
  SR_NAME="$(basename "$SRC_SYSROOT")"
fi
SR_DEST="$PREFIX/sysroots/$SR_NAME"

# Dry-run: print the command, never execute it.
run() {
  if (( DRYRUN )); then
    printf '  would: %s\n' "$*"
  else
    "$@"
  fi
}

if (( DRYRUN )); then
  info "dry-run: nothing will be changed"
fi
if (( UNINSTALL )) && (( ! PREFIX_GIVEN )); then
  die "--uninstall requires --prefix DIR (the default is never assumed for removal)"
fi
if (( ! UNINSTALL )) && [[ ! -f "$REPO/compiler/Makefile" ]]; then
  die "no compiler/Makefile next to this script — run it from a pyc checkout"
fi

pick_cxx() {
  local c
  for c in "${PYC_CXX:-}" clang++-22 clang++; do
    [[ -n "$c" ]] || continue
    if command -v "$c" >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
  done
  return 1
}

step_host() {
  info "1/4 host tools"
  local missing=()
  command -v make >/dev/null 2>&1 || missing+=("make")
  command -v curl >/dev/null 2>&1 || missing+=("curl")
  command -v tar  >/dev/null 2>&1 || missing+=("tar")
  command -v xz   >/dev/null 2>&1 || missing+=("xz")
  CXX="$(pick_cxx || true)"
  if [[ -z "${CXX:-}" ]]; then missing+=("clang++-22 (LLVM 22)"); fi
  if (( ${#missing[@]} > 0 )); then
    if (( DRYRUN )); then
      printf '  note: missing for a real run: %s\n' "${missing[*]}"
      return 0
    fi
    die "missing: ${missing[*]}
  LLVM 22: https://apt.llvm.org (clang++-22, lld)."
  fi
  info "cxx $CXX"
}

step_compiler() {
  info "2/4 compiler -> $PREFIX"
  if [[ -f "$PREFIX/lib/pyc/pyc-prefix.json" ]]; then
    info "reinstalling over a previous pyc install at $PREFIX"
  fi
  run "$REPO/tools/install-pyc.sh" --prefix "$PREFIX" --no-sysroot
}

# A copied sysroot's manifest still names the source tree: rewrite its
# absolute paths to the destination (the same procedure as USAGE.md section 8).
rewrite_manifest() {
  local root="$1" old="$2"
  if (( DRYRUN )); then
    printf '  would: rewrite %s paths (%s -> %s)\n' "$root/pyc-sysroot.json" "$old" "$root"
    return 0
  fi
  python3 - "$root/pyc-sysroot.json" "$old" "$root" <<'PY'
import json, os, sys
path, old, new = sys.argv[1:4]
if not old.endswith("/"):
    old += "/"
data = {}
if os.path.isfile(path):
    data = json.load(open(path))
for k, v in list(data.items()):
    if isinstance(v, str) and v.startswith(old):
        data[k] = new + v[len(old):]
json.dump(data, open(path, "w"), indent=2, sort_keys=True)
open(path, "a").write("\n")
PY
}

step_sysroot() {
  info "3/4 sysroot -> $SR_DEST"
  if [[ -n "$SRC_SYSROOT" ]]; then
    local src
    src="$(cd "$SRC_SYSROOT" 2>/dev/null && pwd)" \
      || die "no such directory: $SRC_SYSROOT"
    if [[ ! -x "$src/bin/python3.14" && ! -x "$src/bin/python3" ]]; then
      die "--sysroot $src has no bin/python3* (not a sysroot)"
    fi
    if (( COPY_SYSROOT )); then
      run rm -rf "$SR_DEST"
      run cp -a "$src" "$SR_DEST"
      rewrite_manifest "$SR_DEST" "$src"
      run "$REPO/tools/install-sysroot.sh" --prefix "$SR_DEST" --verify-only
    else
      if [[ -d "$SR_DEST" && ! -L "$SR_DEST" ]]; then
        die "$SR_DEST exists and is a directory (remove it, or pass --copy-sysroot)"
      fi
      run ln -sfn "$src" "$SR_DEST"
      run "$REPO/tools/install-sysroot.sh" --prefix "$SR_DEST" --verify-only
    fi
  elif (( BUILD_SYSROOT )); then
    run "$REPO/tools/build-python-sysroot.sh" --version 3.14.7 --prefix "$SR_DEST"
  else
    # Global (not local): the EXIT trap that cleans it runs after this
    # function has left the stack, where a local is unbound under set -u.
    if (( DRYRUN )); then
      TMP="/tmp/pyc-install.XXXXXX"
    else
      TMP="$(mktemp -d /tmp/pyc-install.XXXXXX)"
      trap 'rm -rf "$TMP"' EXIT
    fi
    run curl -fL --retry 3 -o "$TMP/sysroot.tar.xz" "$URL"
    run "$REPO/tools/install-sysroot.sh" --archive "$TMP/sysroot.tar.xz" --prefix "$SR_DEST"
  fi
  if [[ ! "$SR_NAME" =~ ^cp.*-tier ]]; then
    info "note: $SR_NAME does not match the prefix auto-search pattern cp*-tier*"
    info "       pyc will not find it without PYC_SYSROOT=$SR_DEST"
  fi
}

step_smoke() {
  info "4/4 smoke"
  if (( NO_SMOKE )); then
    info "skipped (--no-smoke)"
    return 0
  fi
  if (( DRYRUN )); then
    printf '  would: compile+run a program with %s (no PYC_SYSROOT) and\n' "$PREFIX/bin/pyc"
    printf '         compare its output with the sysroot CPython in %s/bin\n' "$SR_DEST"
    return 0
  fi
  local py src out got_pyc got_py
  py="$(compgen -G "$SR_DEST/bin/python3.[0-9]*" | grep -v -- '-config$' | sort -V | tail -1 || true)"
  if [[ -z "$py" || ! -x "$py" ]]; then
    die "no interpreter in $SR_DEST/bin (the sysroot step did not complete?)"
  fi
  src="$(mktemp /tmp/pyc-install-smoke.XXXXXX.py)"
  out="/tmp/pyc-install-smoke-bin"
  printf 'print(2 ** 10)\n' > "$src"
  # No PYC_SYSROOT here on purpose: the prefix's own sysroots/ must
  # resolve (that is the property an installed prefix must have).
  env -u PYC_SYSROOT "$PREFIX/bin/pyc" "$src" -o "$out" -O0
  got_pyc="$("$out")"
  got_py="$(env -u LD_LIBRARY_PATH "$py" "$src")"
  rm -f "$src" "$out"
  if [[ "$got_pyc" != "$got_py" ]]; then
    die "smoke: pyc said '$got_pyc', sysroot CPython said '$got_py'"
  fi
  info "smoke ok: pyc output == sysroot CPython output ('$got_pyc')"
}

do_uninstall() {
  local marker="$PREFIX/lib/pyc/pyc-prefix.json"
  if [[ ! -f "$marker" ]]; then
    if (( ! FORCE )); then
      die "not a pyc install: no $marker
  pass --force to remove the pyc paths anyway, or check --prefix"
    fi
    info "force: no pyc-prefix.json marker at $PREFIX"
  fi
  info "uninstall pyc from $PREFIX"
  if [[ -L "$SR_DEST" ]]; then
    info "sysroot is a symlink -> $(readlink "$SR_DEST") (the target is NOT removed)"
  fi
  run rm -f  "$PREFIX/bin/pyc"
  run rm -rf "$PREFIX/lib/pyc"
  run rm -rf "$SR_DEST"
  if (( ! DRYRUN )); then
    local d
    for d in "$PREFIX/sysroots" "$PREFIX/lib" "$PREFIX/bin"; do
      if [[ -d "$d" && -z "$(ls -A "$d" 2>/dev/null)" ]]; then
        rmdir "$d"
        info "removed empty $d"
      fi
    done
  fi
  info "done (the prefix itself and anything pyc does not own are untouched)"
}

if (( UNINSTALL )); then
  do_uninstall
  exit 0
fi

step_host
step_compiler
step_sysroot
step_smoke

if (( DRYRUN )); then
  cat <<EOF

Dry-run complete: nothing was changed.
Re-run without --dryrun to perform the steps above.
EOF
else
  cat <<EOF

Installed.
  $PREFIX/bin/pyc

  export PATH=$PREFIX/bin:\$PATH
  pyc prog.py -o prog && ./prog

No PYC_SYSROOT needed: the sysroot is inside the prefix ($SR_DEST).
Uninstall: $0 --uninstall --prefix $PREFIX
EOF
fi
