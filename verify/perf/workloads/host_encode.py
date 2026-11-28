# host_encode -- real-app perf workload (STATUS.md 'Real-world
# performance').
#
# Runs the real, dependency-free, ubiquitous third-party package idna
# (vendored under verify/corpus/realapp/) to encode N host names to their
# IDNA/punycode form -- the transform behind every internationalised domain
# name. idna does NFC normalisation, bidi and position checks, and
# label-length checks over each label: call-dense pure Python on a
# different code path than json_validate.
#
# The driver prints a checksum-of-work: the 12-hex sha256 of the vendored
# source (so editing the third-party code changes it and trips perf_run's
# pyc-vs-oracle cross-check), the encoded count, and the first encoded
# label. The output carries NO wall-clock -- perf_run cross-checks stdout
# byte-for-byte and a timing in it would always mismatch.
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_PKG_ROOT = _REPO / "corpus" / "realapp"          # container on sys.path
_PKG_DIR = _PKG_ROOT / "idna"                     # the vendored package
if str(_PKG_ROOT) not in sys.path:
    sys.path.insert(0, str(_PKG_ROOT))

import idna  # noqa: E402

# Work size: tuned to ~1.5 s of encoding on the oracle (STATUS.md).
N = 40000


def package_hash() -> str:
    h = hashlib.sha256()
    for p in sorted(_PKG_DIR.rglob("*.py")):
        h.update(str(p.relative_to(_PKG_DIR)).encode())
        h.update(p.read_bytes())
    return h.hexdigest()[:12]


def hosts(n: int) -> list[str]:
    # Deterministic synthetic host names whose labels are IDNA-valid ASCII.
    return [f"example-yyy{'-' + str(i % 50) + '.com'}" for i in range(n)]


def main() -> None:
    data = hosts(N)
    encoded = [idna.encode(h) for h in data]
    first = encoded[0].decode() if encoded else ""
    print(package_hash(), len(encoded), first)


main()
