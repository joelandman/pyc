"""Corpus sources.

CHARTER I5/I6 — real Python programs, never inline (source, expected) pairs.
That format is what let the old tree's 662-case suite stay green on a subset.

A corpus source yields Cases. It never yields an expected output, because no
component of this harness is permitted to know one.
"""

from __future__ import annotations

import ast
import os
from pathlib import Path
from typing import Iterator

from verify.differential import Case

# CPython's own suite includes files that are not standalone programs, or that
# exist to exercise the interpreter's own internals rather than the language.
# Excluding them is a statement about what the metric measures, so each entry
# needs a reason.
_LIBTEST_SKIP_PREFIXES = (
    "test_capi",          # C-API internals, not the language
    "test_cext",          # builds C extensions at test time
    "test_cppext",
    "test_embed",         # embeds an interpreter
    "test_gdb",           # requires gdb
    "test_tools",         # ships CPython's own dev tooling
    "test_peg_generator", # regenerates CPython's parser
    "test_multiprocess",  # spawns interpreters
    "test_concurrent",
    "test_subprocess",    # re-executes sys.executable
    "test_venv",
    "test_site",
    "test_sysconfig",
    "test_distutils",
    "test_importlib",     # imports by interpreter machinery
    "test_idle",
    "test_tkinter",
    "test_ttk",
    "test_turtle",
)


def _is_parseable(path: Path) -> bool:
    try:
        ast.parse(path.read_text(encoding="utf-8", errors="replace"))
        return True
    except (SyntaxError, ValueError, OSError):
        return False


def from_directory(root: Path, *, pattern: str = "*.py",
                   recursive: bool = False) -> Iterator[Case]:
    """Every .py file under `root` as a standalone program."""
    it = root.rglob(pattern) if recursive else root.glob(pattern)
    for p in sorted(it):
        if p.name.startswith("_") or not p.is_file():
            continue
        yield Case(path=p)


def from_cpython_libtest(stdlib_root: Path, *, limit: int | None = None,
                         include_skipped: bool = False) -> Iterator[Case]:
    """CPython's own `Lib/test/` — the north-star metric (CHARTER I6).

    These are unittest modules. Run directly they execute their own
    `unittest.main()` block if present; many do not, in which case both
    CPython and pyc produce empty output and the case is a trivially-passing
    import test. That is still meaningful: it proves the module's *module-level*
    code — imports, class bodies, decorators, type annotations — compiles and
    runs. Deeper execution comes from running them under a unittest driver,
    which is a later increment.
    """
    testdir = stdlib_root / "test"
    if not testdir.is_dir():
        return
    n = 0
    for p in sorted(testdir.glob("test_*.py")):
        if not include_skipped and p.name.startswith(_LIBTEST_SKIP_PREFIXES):
            continue
        if not _is_parseable(p):
            continue
        yield Case(path=p, name=f"Lib/test/{p.name}")
        n += 1
        if limit is not None and n >= limit:
            return


def resolve_sysroot_oracle(sysroot: Path | None) -> Path | None:
    """Read the interpreter out of a pyc-sysroot.json manifest.

    The parse oracle and the differential oracle must be the same binary — a
    divergence has to be measured against the runtime actually being targeted
    (VERSION_TARGETING.md).
    """
    if sysroot is None:
        return None
    manifest = Path(sysroot) / "pyc-sysroot.json"
    if not manifest.is_file():
        return None
    import json

    data = json.loads(manifest.read_text())
    interp = data.get("interpreter")
    return Path(interp) if interp and os.path.exists(interp) else None
