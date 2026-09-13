#!/usr/bin/env bash
#
# pack-sysroot.sh — tar an existing pyc CPython sysroot (S2).
#
# Does not build CPython. The producer is build-python-sysroot.sh; this only
# packs a prefix that already exists.
#
#   ./tools/pack-sysroot.sh
#   ./tools/pack-sysroot.sh --prefix DIR
#   ./tools/pack-sysroot.sh --prefix DIR -o /tmp/cp314-3.14.7-tier1-x86_64.tar.xz
#   ./tools/pack-sysroot.sh --dry-run
#
set -euo pipefail

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }

PREFIX="${PYC_SYSROOT:-$HOME/opt/py-sysroots/cp314-3.14.7-tier1}"
OUT=""
DRY_RUN=0

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)  PREFIX="${2:?}"; shift 2;;
    -o|--output) OUT="${2:?}"; shift 2;;
    --dry-run) DRY_RUN=1; shift;;
    -h|--help) usage;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

[[ -d "$PREFIX" ]] || die "no sysroot at $PREFIX (build it, or pass --prefix)"
PREFIX="$(cd "$PREFIX" && pwd)"

stamp_identity() {
  local py=""
  py="$(compgen -G "$PREFIX/bin/python3.[0-9]*" | grep -v -- '-config$' | sort -V | tail -1 || true)"
  [[ -n "$py" && -x "$py" ]] || die "no interpreter in $PREFIX/bin"
  local manifest="$PREFIX/pyc-sysroot.json"
  local write=0
  (( DRY_RUN )) || write=1
  "$py" - "$manifest" "$write" <<'PY'
import json, os, platform, sys, sysconfig
path = sys.argv[1]
write = sys.argv[2] == "1"
data = {}
if os.path.isfile(path):
    with open(path) as f:
        data = json.load(f)
abi = "cp%d%d" % sys.version_info[:2]
if sysconfig.get_config_var("Py_GIL_DISABLED"):
    abi += "t"
if hasattr(sys, "gettotalrefcount"):
    abi += "d"
version = data.get("version") or "%d.%d.%d" % sys.version_info[:3]
tier = int(data.get("tier") or 1)
machine = platform.machine() or "unknown"
glibc = None
try:
    glibc = os.confstr("CS_GNU_LIBC_VERSION") or None
except (ValueError, OSError, AttributeError):
    name, ver = platform.libc_ver()
    if name:
        glibc = (name + " " + ver).strip()
archive = "%s-%s-tier%s-%s.tar.xz" % (abi, version, tier, machine)
libpl = sysconfig.get_config_var("LIBPL")
libdir = sysconfig.get_config_var("LIBDIR")
library = sysconfig.get_config_var("LIBRARY")
ldlibrary = sysconfig.get_config_var("LDLIBRARY")
static = os.path.join(libpl, library) if libpl and library else ""
shared = os.path.join(libdir, ldlibrary) if libdir and ldlibrary else ""
data["abi"] = abi
data["version"] = version
data["xy"] = "%d.%d" % sys.version_info[:2]
data["tier"] = tier
data["machine"] = machine
data["glibc"] = glibc
data["archive_name"] = archive
data["sysroot"] = sys.prefix
data["interpreter"] = sys.executable
data["include"] = sysconfig.get_paths().get("include")
data["libpython_static"] = static if static and os.path.exists(static) else None
data["libpython_shared"] = shared if shared and os.path.exists(shared) else None
if write:
    with open(path, "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")
print(archive)
print(machine)
print(glibc or "")
print(abi)
print(version)
print(str(tier))
PY
}

IDENTITY="$(stamp_identity)" || die "failed to stamp $PREFIX/pyc-sysroot.json"
ARCHIVE_NAME="$(printf '%s\n' "$IDENTITY" | sed -n '1p')"
MACHINE="$(printf '%s\n' "$IDENTITY" | sed -n '2p')"
GLIBC="$(printf '%s\n' "$IDENTITY" | sed -n '3p')"
ABI="$(printf '%s\n' "$IDENTITY" | sed -n '4p')"
VERSION="$(printf '%s\n' "$IDENTITY" | sed -n '5p')"
TIER="$(printf '%s\n' "$IDENTITY" | sed -n '6p')"

PARENT="$(dirname "$PREFIX")"
BASE="$(basename "$PREFIX")"
[[ -n "$OUT" ]] || OUT="${PARENT}/${ARCHIVE_NAME}"

info "abi       $ABI"
info "version   $VERSION"
info "tier      $TIER"
info "machine   $MACHINE"
info "glibc     ${GLIBC:-unknown}"
info "archive   $OUT"

if (( DRY_RUN )); then
  printf '  would: tar -C %q -cJf %q --exclude=__pycache__ --exclude=*.pyc %q\n' \
    "$PARENT" "$OUT" "$BASE"
  exit 0
fi

mkdir -p "$(dirname "$OUT")"
tar -C "$PARENT" -cJf "$OUT" \
  --exclude='__pycache__' \
  --exclude='*.pyc' \
  "$BASE"
info "wrote $OUT ($(wc -c < "$OUT") bytes)"
