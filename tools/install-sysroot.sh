#!/usr/bin/env bash
#
# install-sysroot.sh — unpack a packed pyc sysroot and rewrite absolute paths (S2).
#
#   ./tools/install-sysroot.sh --archive FILE --prefix DIR
#   ./tools/install-sysroot.sh --prefix DIR --verify-only
#
# --prefix is the sysroot directory itself (bin/python3.X lives there).
# After unpack, pyc-sysroot.json interpreter/sysroot/include/libpython_* are
# rewritten from the packed prefix to DIR so pycc does not need the old path.
#
set -euo pipefail

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }

ARCHIVE=""
PREFIX=""
VERIFY_ONLY=0

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --archive) ARCHIVE="${2:?}"; shift 2;;
    --prefix)  PREFIX="${2:?}"; shift 2;;
    --verify-only) VERIFY_ONLY=1; shift;;
    -h|--help) usage;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

[[ -n "$PREFIX" ]] || die "--prefix DIR is required"
PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"

rewrite_manifest() {
  local root="$1"
  local manifest="$root/pyc-sysroot.json"
  local py=""
  py="$(compgen -G "$root/bin/python3.[0-9]*" | grep -v -- '-config$' | sort -V | tail -1 || true)"
  [[ -n "$py" && -x "$py" ]] || die "no interpreter in $root/bin after unpack"
  "$py" - "$manifest" "$root" <<'PY'
import json, os, sys
path, new = sys.argv[1], os.path.abspath(sys.argv[2])
data = {}
if os.path.isfile(path):
    with open(path) as f:
        data = json.load(f)
old = data.get("sysroot") or ""
if old.endswith("/"):
    old = old[:-1]
def rewrite(v):
    if not isinstance(v, str) or not old:
        return v
    if v == old:
        return new
    if v.startswith(old + "/"):
        return new + v[len(old):]
    return v
if old:
    for k, v in list(data.items()):
        data[k] = rewrite(v)
data["sysroot"] = new
data["interpreter"] = os.path.join(new, "bin", os.path.basename(data.get("interpreter") or "python3"))
if not os.path.isfile(data["interpreter"]):
    # keep a real executable even if basename guess failed
    for name in sorted(os.listdir(os.path.join(new, "bin"))):
        if name.startswith("python3.") and not name.endswith("-config"):
            cand = os.path.join(new, "bin", name)
            if os.access(cand, os.X_OK):
                data["interpreter"] = cand
                break
with open(path, "w") as f:
    json.dump(data, f, indent=2, sort_keys=True)
    f.write("\n")
print(data["interpreter"])
print(data.get("sysroot", ""))
PY
}

verify_sysroot() {
  local root="$1"
  local py=""
  py="$(compgen -G "$root/bin/python3.[0-9]*" | grep -v -- '-config$' | sort -V | tail -1 || true)"
  [[ -n "$py" && -x "$py" ]] || die "no interpreter in $root/bin"
  local py_clean=(env -u LD_LIBRARY_PATH "$py")
  "${py_clean[@]}" -c 'pass' 2>/dev/null || die \
    "interpreter cannot run without LD_LIBRARY_PATH — RPATH missing, sysroot is not self-contained"
  local got want="15511210043330985984000000"
  got="$("${py_clean[@]}" -c 'import math;print(math.factorial(25))')"
  [[ "$got" == "$want" ]] || die "factorial(25)=$got (want $want) — CHARTER I2"
  local ident
  ident="$("${py_clean[@]}" - "$root/pyc-sysroot.json" <<'PY'
import json, os, platform, sys
path = sys.argv[1]
host = platform.machine() or "unknown"
data = {}
if os.path.isfile(path):
    data = json.load(open(path))
arch = data.get("machine") or ""
glibc = data.get("glibc") or "unknown"
if arch and host and arch != host:
    sys.stderr.write("machine mismatch: archive %s host %s\n" % (arch, host))
    sys.exit(2)
print("machine %s  glibc %s" % (host, glibc))
PY
  )" || die "sysroot identity check failed"
  info "verify ok  $py  factorial(25) matches  $ident"
}

if (( VERIFY_ONLY )); then
  [[ -d "$PREFIX" ]] || die "no sysroot at $PREFIX"
  verify_sysroot "$PREFIX"
  exit 0
fi

[[ -n "$ARCHIVE" ]] || die "--archive FILE is required (or pass --verify-only)"
[[ -f "$ARCHIVE" ]] || die "no archive $ARCHIVE"

info "unpacking $ARCHIVE -> $PREFIX"
rm -rf "$PREFIX"
mkdir -p "$PREFIX"
# pack-sysroot.sh emits one top-level directory; strip it so --prefix is the tree.
tar -xJf "$ARCHIVE" -C "$PREFIX" --strip-components=1
rewritten="$(rewrite_manifest "$PREFIX")"
info "rewrote $PREFIX/pyc-sysroot.json"
info "interpreter $(printf '%s\n' "$rewritten" | sed -n '1p')"
verify_sysroot "$PREFIX"
