#!/usr/bin/env bash
#
# install-sysroot.sh — unpack a packed pyc sysroot and rewrite absolute paths (S2).
#
#   ./tools/install-sysroot.sh --archive FILE --prefix DIR
#   ./tools/install-sysroot.sh --prefix DIR --verify-only
#
# --prefix is the sysroot directory itself (bin/python3.X lives there).
# After unpack, pyc-sysroot.json interpreter/sysroot/include/libpython_* are
# rewritten from the packed prefix to DIR so pyc does not need the old path.
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

  # Every measurement below runs with LD_LIBRARY_PATH scrubbed AND pinned to
  # the tree's own lib directory. Scrubbing alone is not enough: the
  # launcher's RUNPATH may not exist on this machine (trees packed before
  # 2026-11-10 carry the CI build path, and a fresh local build carries its
  # build prefix), and then the loader falls back to a SYSTEM libpython3.X —
  # measured 2026-11-10 on hal9k, a sound 3.14.7 tree "reported" 3.14.4 that
  # way, and its identity check below refused a good asset. Worse, the
  # version banner and the sysconfig data can come from DIFFERENT
  # interpreters (core from the host lib, data from the tree's stdlib), so a
  # check that measures the host is not measuring the sysroot at all (I5).
  local py_own=(env -u LD_LIBRARY_PATH LD_LIBRARY_PATH="$root/lib" "$py")
  "${py_own[@]}" -c 'pass' 2>/dev/null || die \
    "interpreter cannot run against its own lib: $root/lib (sysroot is broken)"

  # The manifest's shared libpython, when named, must exist inside the tree.
  # This is what makes py_own above trustworthy: if the tree's own lib were
  # missing, the pinned run could still fall through to a host libpython and
  # pass every check while the sysroot itself is hollow.
  if ! "${py_own[@]}" - "$root/pyc-sysroot.json" <<'PY'
import json, os, sys
path = sys.argv[1]
data = {}
if os.path.isfile(path):
    data = json.load(open(path))
shared = data.get("libpython_shared")
if shared:
    # realpath: a symlinked sysroot's manifest points at the tree the
    # link targets, and that is where self-containment must hold.
    root = os.path.dirname(os.path.realpath(path))
    if not (os.path.isfile(shared) and shared.startswith(root + os.sep)):
        sys.stderr.write(
            "manifest names libpython_shared %r, which is missing or outside "
            "the sysroot tree — the tree is not self-contained\n" % shared)
        sys.exit(4)
PY
  then
    die "sysroot self-containment check failed (see above)"
  fi

  # Relocatability: warn, not die. A tree whose RUNPATH does not resolve on
  # this machine (the pre-2026-11-10 published assets) still works via the
  # pin above — pyc pins $root/lib for all its oracle subprocesses — but a
  # user who runs this interpreter directly would silently get a host
  # CPython on a box that carries one.
  if ! env -u LD_LIBRARY_PATH "$py" -c 'pass' 2>/dev/null; then
    warn "interpreter only runs via LD_LIBRARY_PATH=$root/lib on this machine:
  $py carries a RUNPATH that does not exist here (pre-2026-11-10 tree).
  pyc pins $root/lib for its own subprocesses; when you run $py directly,
  set LD_LIBRARY_PATH=$root/lib (see USAGE.md section 5.4)."
  fi

  local got want="15511210043330985984000000"
  got="$("${py_own[@]}" -c 'import math;print(math.factorial(25))')"
  [[ "$got" == "$want" ]] || die "factorial(25)=$got (want $want) — CHARTER I2"
  local ident
  ident="$("${py_own[@]}" - "$root/pyc-sysroot.json" <<'PY'
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
ver = "%d.%d.%d" % sys.version_info[:3]
claimed = data.get("version") or ""
if claimed and claimed != ver:
    # The one check the published 3.14.5-in-a-3.14.7-manifest release
    # slipped through: a sysroot whose interpreter does not match its
    # manifest is not a sysroot, whatever else it contains.
    sys.stderr.write(
        "version mismatch: manifest claims %s, interpreter is %s — this "
        "sysroot was stamped against a different CPython build\n"
        % (claimed, ver))
    sys.exit(3)
print("machine %s  glibc %s  version %s" % (host, glibc, ver))
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
