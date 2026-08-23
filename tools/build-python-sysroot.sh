#!/usr/bin/env bash
#
# build-python-sysroot.sh — build a CPython sysroot for pyc.
#
# A pyc target is data, not code: a sysroot keyed by (version, ABI, link tier).
# See rebuild/VERSION_TARGETING.md and rebuild/CHARTER.md (I7, I8).
#
#   Tier 1  static libpython + dynamic libc, linked with -rdynamic.
#           dlopen works, so C-extension wheels (NumPy, PyTorch) work.
#           This is the default and what `--static` currently means.
#
#   Tier 2  fully static (-static). No dlopen at all, so stdlib extension
#           modules must be compiled into libpython (MODULE_BUILDTYPE=static).
#           Can NEVER load a C-extension wheel — that is ELF, not a gap.
#           DEFERRED by decision 2026-08-22; build only when explicitly asked.
#
# Usage:
#   ./tools/build-python-sysroot.sh                        # tier 1, default version
#   ./tools/build-python-sysroot.sh --version 3.13.7
#   ./tools/build-python-sysroot.sh --tier 2               # deferred; needs --force-tier2
#   ./tools/build-python-sysroot.sh --freethreaded         # cp314t ABI
#   ./tools/build-python-sysroot.sh --verify-only --prefix DIR
#   ./tools/build-python-sysroot.sh --require-wheel        # fail unless a wheel loads
#   ./tools/build-python-sysroot.sh --no-wheel-check       # skip (offline / fast)
#   ./tools/build-python-sysroot.sh --check-deps
#   ./tools/build-python-sysroot.sh --dry-run
#
set -euo pipefail

# Single scratch dir with one EXIT trap. Do NOT use a RETURN trap inside a
# function: in bash that installs a GLOBAL trap which then fires on every later
# function return, referencing an out-of-scope variable and tripping `set -u`.
SCRATCH="$(mktemp -d)"
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT

# ---------------------------------------------------------------- defaults --

DEFAULT_VERSION="3.14.7"
SYSROOT_ROOT="${PYC_SYSROOT_ROOT:-$HOME/opt/py-sysroots}"
SRC_ROOT="${PYC_SRC_ROOT:-$HOME/build/cpython-sysroot}"

VERSION="$DEFAULT_VERSION"
TIER=1
FREETHREADED=0
PGO=1
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
PREFIX=""
DRY_RUN=0
VERIFY_ONLY=0
CHECK_DEPS_ONLY=0
FORCE=0
FORCE_TIER2=0
KEEP_BUILD=0
WHEEL_VERIFIED=""
WHEEL_CHECK=1
WHEEL_REQUIRED=0
WHEEL_PKG="numpy"

# ------------------------------------------------------------------- usage --

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
warn() { printf '\033[33mwarn:\033[0m %s\n'  "$*" >&2; }
info() { printf '\033[36m==>\033[0m %s\n'    "$*"; }
step() { printf '\n\033[1m%s\033[0m\n' "$*"; }

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)       VERSION="${2:?--version needs X.Y.Z}"; shift 2;;
    --tier)          TIER="${2:?--tier needs 1 or 2}"; shift 2;;
    --prefix)        PREFIX="${2:?--prefix needs a path}"; shift 2;;
    --sysroot-root)  SYSROOT_ROOT="${2:?}"; shift 2;;
    --src-root)      SRC_ROOT="${2:?}"; shift 2;;
    --jobs|-j)       JOBS="${2:?}"; shift 2;;
    --freethreaded)  FREETHREADED=1; shift;;
    --no-pgo)        PGO=0; shift;;
    --dry-run)       DRY_RUN=1; shift;;
    --verify-only)   VERIFY_ONLY=1; shift;;
    --check-deps)    CHECK_DEPS_ONLY=1; shift;;
    --force)         FORCE=1; shift;;
    --force-tier2)   FORCE_TIER2=1; shift;;
    --keep-build)    KEEP_BUILD=1; shift;;
    --no-wheel-check) WHEEL_CHECK=0; shift;;
    --require-wheel) WHEEL_REQUIRED=1; shift;;
    --wheel-package) WHEEL_PKG="${2:?--wheel-package needs a name}"; shift 2;;
    -h|--help)       usage;;
    *)               die "unknown option: $1 (try --help)";;
  esac
done

[[ "$VERSION" =~ ^3\.([0-9]+)\.([0-9]+)$ ]] || die "version must look like 3.14.7, got '$VERSION'"
XY="${VERSION%.*}"                 # 3.14
MINOR="${BASH_REMATCH[1]}"         # 14
[[ "$TIER" == 1 || "$TIER" == 2 ]] || die "--tier must be 1 or 2"

if (( MINOR < 13 )) && (( FREETHREADED )); then
  die "--freethreaded requires CPython 3.13+; $VERSION does not support --disable-gil"
fi

# ABI tag: cp314 or cp314t (free-threaded is a DISTINCT ABI with distinct
# wheel tags — not a flag that can be added to an existing sysroot later).
ABI="cp${XY//./}"
(( FREETHREADED )) && ABI="${ABI}t"
SYSROOT_NAME="${ABI}-${VERSION}-tier${TIER}"
[[ -n "$PREFIX" ]] || PREFIX="${SYSROOT_ROOT}/${SYSROOT_NAME}"

TARBALL="Python-${VERSION}.tar.xz"
SRC_DIR="${SRC_ROOT}/Python-${VERSION}"
BUILD_DIR="${SRC_DIR}/build-${SYSROOT_NAME}"
URL="https://www.python.org/ftp/python/${VERSION}/${TARBALL}"

run() {
  if (( DRY_RUN )); then printf '  \033[90m$ %s\033[0m\n' "$*"; else eval "$@"; fi
}

# ------------------------------------------------------------ dependencies --

check_deps() {
  step "Checking build dependencies"
  local missing=() opt_missing=()

  for t in gcc make curl tar pkg-config; do
    command -v "$t" >/dev/null 2>&1 || missing+=("$t")
  done

  # Header checks. Without these CPython still builds, but silently omits the
  # corresponding stdlib module — which for pyc means an ImportError at run
  # time in a compiled binary. Treat ssl/zlib/ffi as required.
  local -A req=(
    [zlib]="zlib.h"        [openssl]="openssl/ssl.h"
    [libffi]="ffi.h"
  )
  local -A opt=(
    [bzip2]="bzlib.h"      [liblzma]="lzma.h"
    [sqlite3]="sqlite3.h"  [readline]="readline/readline.h"
    [ncurses]="curses.h"   [uuid]="uuid/uuid.h"
    [gdbm]="gdbm.h"        [tk]="tk.h"
  )
  local probe="$SCRATCH/deps"; mkdir -p "$probe"

  _has_header() {
    echo "#include <$1>" > "$probe/t.c"
    gcc -fsyntax-only $(pkg-config --cflags "$2" 2>/dev/null || true) \
        "$probe/t.c" >/dev/null 2>&1
  }
  for p in "${!req[@]}"; do _has_header "${req[$p]}" "$p" || missing+=("$p (${req[$p]})"); done
  for p in "${!opt[@]}"; do _has_header "${opt[$p]}" "$p" || opt_missing+=("$p"); done

  if (( ${#opt_missing[@]} )); then
    warn "optional deps missing — the matching stdlib modules will be ABSENT"
    warn "  ${opt_missing[*]}"
  fi
  if (( ${#missing[@]} )); then
    printf '\n'
    die "missing required build dependencies:
    ${missing[*]}

  Debian/Ubuntu:
    sudo apt build-dep python3
    sudo apt install build-essential pkg-config zlib1g-dev libssl-dev libffi-dev \\
         libbz2-dev liblzma-dev libsqlite3-dev libreadline-dev libncurses-dev \\
         uuid-dev libgdbm-dev tk-dev

  Fedora/RHEL:
    sudo dnf builddep python3
    sudo dnf install gcc make pkgconf-pkg-config zlib-devel openssl-devel \\
         libffi-devel bzip2-devel xz-devel sqlite-devel readline-devel \\
         ncurses-devel libuuid-devel gdbm-devel tk-devel"
  fi
  info "all required dependencies present"
}

# ---------------------------------------------------------------- fetch src --

fetch_source() {
  step "Fetching CPython ${VERSION}"
  run "mkdir -p '$SRC_ROOT'"

  if [[ -d "$SRC_DIR" && $FORCE -eq 0 ]]; then
    info "source tree already present: $SRC_DIR (use --force to re-extract)"
    return
  fi

  if [[ ! -f "${SRC_ROOT}/${TARBALL}" ]]; then
    info "downloading $URL"
    run "curl -fSL --retry 3 -o '${SRC_ROOT}/${TARBALL}.part' '$URL'"
    run "mv '${SRC_ROOT}/${TARBALL}.part' '${SRC_ROOT}/${TARBALL}'"
  else
    info "tarball cached: ${SRC_ROOT}/${TARBALL}"
  fi

  # Transport trust is HTTPS to python.org. Record the digest so a rebuild on
  # another machine can be compared against this one.
  if (( ! DRY_RUN )); then
    local sum; sum="$(sha256sum "${SRC_ROOT}/${TARBALL}" | cut -d' ' -f1)"
    info "sha256 ${sum}"
    if command -v python3 >/dev/null && python3 -c 'import sigstore' 2>/dev/null; then
      info "verifying sigstore bundle"
      curl -fsSL -o "${SRC_ROOT}/${TARBALL}.sigstore" "${URL}.sigstore" 2>/dev/null &&
        python3 -m sigstore verify identify \
          --bundle "${SRC_ROOT}/${TARBALL}.sigstore" \
          --cert-identity-regexp '.*' --cert-oidc-issuer-regexp '.*' \
          "${SRC_ROOT}/${TARBALL}" 2>/dev/null ||
        warn "sigstore verification unavailable or failed (continuing on HTTPS trust)"
    else
      warn "python 'sigstore' package not installed — signature NOT verified (HTTPS trust only)"
    fi
  fi

  run "rm -rf '$SRC_DIR'"
  run "tar -xf '${SRC_ROOT}/${TARBALL}' -C '$SRC_ROOT'"

  if (( ! DRY_RUN )); then
    local got
    got="$(sed -n 's/^#define PY_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' \
           "$SRC_DIR/Include/patchlevel.h")"
    [[ "$got" == "$VERSION" ]] || die "tarball declares $got, expected $VERSION"
    info "source verified: PY_VERSION $got"
  fi
}

# ------------------------------------------------------------------ config --

configure_build() {
  step "Configuring ${SYSROOT_NAME}"

  local args=(
    "--prefix=$PREFIX"
    "--with-ensurepip=install"
  )
  # A sysroot must be self-contained: the installed interpreter has to find its
  # own libpython without help from the environment. Without this RPATH the
  # binary dies with "libpython3.X.so.1.0: cannot open shared object file" on
  # any machine that does not happen to carry $PREFIX/lib in LD_LIBRARY_PATH --
  # which is precisely the portability bug this script exists to avoid.
  local env_prefix=("LDFLAGS=-Wl,-rpath,${PREFIX}/lib")

  if (( TIER == 1 )); then
    # Shared libpython for the dynamic binary; the static libpython3.X.a comes
    # along automatically (--with-static-libpython defaults to yes) and lands
    # in LIBPL, not lib/. That .a plus -rdynamic is Tier 1.
    args+=("--enable-shared")
  else
    # Tier 2: no shared libpython, and every stdlib extension module compiled
    # INTO libpython. Without MODULE_BUILDTYPE=static a -static binary links
    # and starts, then dies on `import math` — 77 of the stdlib's C modules
    # are dlopen'ed .so in a stock build.
    args+=("--disable-shared")
    env_prefix+=("MODULE_BUILDTYPE=static")
  fi

  args+=("--with-static-libpython")   # explicit; the default, but load-bearing

  if (( PGO )); then
    args+=("--enable-optimizations" "--with-lto")
  fi
  if (( FREETHREADED )); then
    args+=("--disable-gil")
  fi

  run "mkdir -p '$BUILD_DIR'"
  info "prefix:  $PREFIX"
  info "configure: ${env_prefix[*]:-} ../configure ${args[*]}"
  run "cd '$BUILD_DIR' && ${env_prefix[*]:-} ../configure ${args[*]}"
}

build_install() {
  step "Building (jobs=$JOBS)"
  if (( PGO )); then
    info "PGO + LTO enabled — expect 20-40 min; use --no-pgo for a ~4 min build"
  fi
  run "make -C '$BUILD_DIR' -j'$JOBS'"

  step "Installing to $PREFIX"
  # 'altinstall' would skip the unversioned python3 symlink; a sysroot is
  # self-contained and never on PATH, so a plain install is what we want.
  run "make -C '$BUILD_DIR' install"
}

# ------------------------------------------------------------------ verify --

write_manifest() {
  local py="$PREFIX/bin/python${XY}"
  local manifest="$PREFIX/pyc-sysroot.json"
  info "writing $manifest"
  (( DRY_RUN )) && return 0

  "$py" - "$manifest" "$ABI" "$TIER" "$VERSION" "$WHEEL_VERIFIED" <<'PYEOF'
import json, sys, sysconfig, os
manifest, abi, tier, version = sys.argv[1:5]
wheel_verified = sys.argv[5] if len(sys.argv) > 5 else ''
libpl = sysconfig.get_config_var('LIBPL')
libdir = sysconfig.get_config_var('LIBDIR')
static = os.path.join(libpl, sysconfig.get_config_var('LIBRARY'))
shared = os.path.join(libdir, sysconfig.get_config_var('LDLIBRARY'))
ptd = {
    "version":        version,
    "xy":             '.'.join(map(str, sys.version_info[:2])),
    "abi":            abi,
    "tier":           int(tier),
    "free_threaded":  bool(sysconfig.get_config_var('Py_GIL_DISABLED')),
    "sysroot":        sys.prefix,
    "interpreter":    sys.executable,
    "include":        sysconfig.get_paths()['include'],
    "libpython_static": static if os.path.exists(static) else None,
    "libpython_shared": shared if os.path.exists(shared) else None,
    "link_flags":     ["-rdynamic"] if int(tier) == 1 else ["-static"],
    "wheels_supported": int(tier) == 1,
    "wheel_verified": wheel_verified or None,
    "builtin_modules":  len(sys.builtin_module_names),
}
try:
    dynload = sysconfig.get_config_var('DESTSHARED')
    ptd["dynload_modules"] = len(os.listdir(dynload)) if dynload and os.path.isdir(dynload) else 0
except OSError:
    ptd["dynload_modules"] = 0
with open(manifest, 'w') as f:
    json.dump(ptd, f, indent=2, sort_keys=True)
    f.write('\n')
print(json.dumps(ptd, indent=2, sort_keys=True))
PYEOF
}

verify() {
  step "Verifying ${SYSROOT_NAME}"
  local py="$PREFIX/bin/python${XY}"

  if (( DRY_RUN )); then info "(dry run — skipping verification)"; return 0; fi
  [[ -x "$py" ]] || die "no interpreter at $py"

  # Verify with LD_LIBRARY_PATH scrubbed. Otherwise a developer machine that
  # already exports $PREFIX/lib will report a sysroot healthy that is broken
  # everywhere else.
  local py_clean=(env -u LD_LIBRARY_PATH "$py")
  "${py_clean[@]}" -c 'pass' 2>/dev/null || die \
    "interpreter cannot run without LD_LIBRARY_PATH set — the RPATH is missing,
  so this sysroot is not self-contained and will fail on other machines"

  # 1. interpreter runs, and int is arbitrary precision (CHARTER I2)
  local got want="15511210043330985984000000"
  got="$("${py_clean[@]}" -c 'import math;print(math.factorial(25))')"
  [[ "$got" == "$want" ]] || die "interpreter sanity failed: factorial(25) = $got"
  info "interpreter OK — $("$py" -V 2>&1)"

  # 2. the static library exists where we expect (LIBPL, not lib/)
  local libpl static_lib
  libpl="$("$py" -c 'import sysconfig;print(sysconfig.get_config_var("LIBPL"))')"
  static_lib="$libpl/$("$py" -c 'import sysconfig;print(sysconfig.get_config_var("LIBRARY"))')"
  [[ -f "$static_lib" ]] || die "static libpython missing: $static_lib"
  info "static libpython: $static_lib ($(du -h "$static_lib" | cut -f1))"

  # 3. the tier's link mode actually produces a working binary. This is the
  #    check that matters — file existence proves nothing.
  local probe="$SCRATCH/verify"; mkdir -p "$probe"
  cat > "$probe/emb.c" <<'CEOF'
#define PY_SSIZE_T_CLEAN
#include <Python.h>
int main(void){
    Py_Initialize();
    if (PyRun_SimpleString("import math,json;print('embed OK',math.factorial(20))")) return 1;
    return Py_FinalizeEx() < 0;
}
CEOF
  local inc; inc="$("$PREFIX/bin/python${XY}-config" --includes)"
  local libs="-lm -lpthread -ldl -lutil -lz"

  if (( TIER == 1 )); then
    info "linking Tier-1 probe (static libpython, -rdynamic)"
    gcc -rdynamic $inc "$probe/emb.c" "$static_lib" $libs -o "$probe/emb" \
      || die "Tier-1 link failed"
    "$probe/emb" | grep -q 'embed OK' || die "Tier-1 probe ran but produced no output"
    ldd "$probe/emb" | grep -qi 'libpython' \
      && die "Tier-1 probe still has a libpython dynamic dependency" \
      || info "Tier-1 verified: no libpython dependency, extensions importable"
  else
    info "linking Tier-2 probe (fully static)"
    gcc -static $inc "$probe/emb.c" "$static_lib" $libs -o "$probe/emb" 2>/dev/null \
      || die "Tier-2 link failed"
    file "$probe/emb" | grep -q 'statically linked' || die "Tier-2 probe is not static"
    "$probe/emb" | grep -q 'embed OK' \
      || die "Tier-2 probe cannot import stdlib extensions — MODULE_BUILDTYPE=static did not take"
    info "Tier-2 verified: fully static, stdlib extensions builtin"
    warn "Tier-2 binaries can NEVER load C-extension wheels (CHARTER I7)"
  fi

  verify_wheel
  write_manifest
}


# CHARTER I7: "a binary that imports a real precompiled wheel". This is the
# capability the entire libpython decision was made to buy, so every sysroot
# build proves it rather than assuming it.
verify_wheel() {
  if (( TIER != 1 )); then
    info "wheel check skipped: a Tier-2 binary cannot load C-extension wheels"
    info "  by construction — no dynamic symbol table (CHARTER I7)"
    return 0
  fi
  (( WHEEL_CHECK )) || { info "wheel check skipped (--no-wheel-check)"; return 0; }

  step "Verifying C-extension wheel support (${WHEEL_PKG})"
  local py="$PREFIX/bin/python${XY}"

  # No network is an environment problem, not a sysroot defect, so this warns
  # by default. --require-wheel makes it fatal; that is what CI should use.
  if ! "$py" -m pip install --quiet --no-input --disable-pip-version-check \
        "$WHEEL_PKG" >/dev/null 2>&1; then
    (( WHEEL_REQUIRED )) && die "cannot install '$WHEEL_PKG' into the sysroot"
    warn "cannot install '$WHEEL_PKG' (offline?) — wheel support UNVERIFIED"
    warn "  re-run with --require-wheel where it must be proven"
    return 0
  fi

  local wheel_ver
  wheel_ver="$("$py" -c "import ${WHEEL_PKG} as m;print(m.__version__)" 2>/dev/null || echo '?')"
  info "installed ${WHEEL_PKG} ${wheel_ver} into the sysroot"

  local probe="$SCRATCH/wheel"; mkdir -p "$probe"
  cat > "$probe/w.c" <<CEOF
#define PY_SSIZE_T_CLEAN
#include <Python.h>
int main(void){
    Py_Initialize();
    if (PyRun_SimpleString(
        "import ${WHEEL_PKG} as m\n"
        "print('wheel-ok', m.__name__, m.__version__)\n")) return 1;
    return Py_FinalizeEx() < 0;
}
CEOF
  local inc libpl static_lib
  inc="$("$PREFIX/bin/python${XY}-config" --includes)"
  libpl="$("$py" -c 'import sysconfig;print(sysconfig.get_config_var("LIBPL"))')"
  static_lib="$libpl/$("$py" -c 'import sysconfig;print(sysconfig.get_config_var("LIBRARY"))')"

  # The product's own link mode: static libpython plus -rdynamic, so the
  # wheel's .so can resolve libpython symbols out of the executable.
  if ! gcc -rdynamic $inc "$probe/w.c" "$static_lib" \
        -lm -lpthread -ldl -lutil -lz -o "$probe/w" 2>/dev/null; then
    (( WHEEL_REQUIRED )) && die "wheel probe failed to link"
    warn "wheel probe failed to link — UNVERIFIED"; return 0
  fi

  if env -u LD_LIBRARY_PATH "$probe/w" 2>/dev/null | grep -q 'wheel-ok'; then
    if ldd "$probe/w" | grep -qi libpython; then
      die "wheel probe has a libpython dynamic dependency — that is not Tier 1"
    fi
    info "Tier-1 binary imported ${WHEEL_PKG} ${wheel_ver} with NO libpython dep"
    WHEEL_VERIFIED="${WHEEL_PKG} ${wheel_ver}"
  else
    (( WHEEL_REQUIRED )) && die "Tier-1 binary could not import '$WHEEL_PKG'"
    warn "Tier-1 binary could not import '$WHEEL_PKG' — UNVERIFIED"
  fi
}

# -------------------------------------------------------------------- main --

main() {
  if (( CHECK_DEPS_ONLY )); then check_deps; exit 0; fi

  if (( VERIFY_ONLY )); then
    [[ -d "$PREFIX" ]] || die "no sysroot at $PREFIX"
    verify; exit 0
  fi

  if (( TIER == 2 && ! FORCE_TIER2 )); then
    die "Tier 2 is DEFERRED by decision (rebuild/CHARTER.md I7).

  Tier 1 unblocks all near-term work including the wheel milestone, and a
  Tier-2 binary can never load a C-extension wheel. Revisit only after a
  wheel is demonstrably loading. Pass --force-tier2 to build it anyway."
  fi

  if [[ -d "$PREFIX" && $FORCE -eq 0 ]]; then
    die "sysroot already exists: $PREFIX (use --force to rebuild, or --verify-only)"
  fi

  printf '\033[1mpyc CPython sysroot build\033[0m\n'
  printf '  version   %s\n  abi       %s\n  tier      %s (%s)\n  prefix    %s\n  jobs      %s\n  pgo       %s\n' \
    "$VERSION" "$ABI" "$TIER" \
    "$( ((TIER==1)) && echo 'static libpython + dynamic libc; wheels OK' || echo 'fully static; NO wheels')" \
    "$PREFIX" "$JOBS" "$( ((PGO)) && echo 'on (PGO+LTO)' || echo 'off')"

  check_deps
  fetch_source
  configure_build
  build_install
  verify

  step "Done"
  cat <<EOF
Sysroot:  $PREFIX
Manifest: $PREFIX/pyc-sysroot.json

Use with pyc:
  pyc prog.py --python=${XY} --python-sysroot=$PREFIX
EOF
}

main "$@"
