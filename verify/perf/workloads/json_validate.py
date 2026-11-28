# json_validate -- real-app perf workload (STATUS.md 'Real-world
# performance').
#
# Runs the real, dependency-free third-party package fastjsonschema
# (vendored under verify/corpus/realapp/) to validate a deterministically
# generated dataset: compile a validator once, then validate N records.
# fastjsonschema's validator is itself generated at runtime and executed
# under the active interpreter, so this measures pyc on the library AND on
# the code it generates -- both under pyc in the pyc run.
#
# The driver prints a checksum-of-work: the 12-hex sha256 of the vendored
# source (so editing the third-party code changes it and trips perf_run's
# pyc-vs-oracle cross-check), the record count, and the corpus length. The
# output carries NO wall-clock -- perf_run cross-checks stdout byte-for-byte
# and a timing in it would always mismatch.
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_PKG_ROOT = _REPO / "corpus" / "realapp"          # container on sys.path
_PKG_DIR = _PKG_ROOT / "fastjsonschema"           # the vendored package
if str(_PKG_ROOT) not in sys.path:
    sys.path.insert(0, str(_PKG_ROOT))

import fastjsonschema  # noqa: E402

# Work size: tuned to ~1.5 s of validation on the oracle (STATUS.md).
N = 700000

SCHEMA = {
    "type": "object",
    "required": ["a", "b", "c"],
    "properties": {
        "a": {"type": "number"},
        "b": {"type": "string"},
        "c": {"type": "integer", "minimum": 0, "maximum": 100},
    },
}


def package_hash() -> str:
    h = hashlib.sha256()
    for p in sorted(_PKG_DIR.rglob("*.py")):
        h.update(str(p.relative_to(_PKG_DIR)).encode())
        h.update(p.read_bytes())
    return h.hexdigest()[:12]


def dataset(n: int) -> list[dict]:
    # Built in memory from fixed patterns: identical input on every run and
    # every machine, no file, no clock, no randomness (perf.py docstring /
    # CHARTER I5). c stays within [0, 100] so every record validates.
    return [{"a": i * 0.5, "b": f"rec-{i}", "c": i % 101} for i in range(n)]


def main() -> None:
    validator = fastjsonschema.compile(SCHEMA)
    data = dataset(N)
    total = 0
    for record in data:
        validator(record)
        total += 1
    print(package_hash(), total, len(data))


main()
