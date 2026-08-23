#!/usr/bin/env python3
"""Generate the C-API symbol table (INTERFACES.md §4) from CPython's own data.

    ./compiler/tools/gen_capi_table.py <cpython-src>/Doc/data/refcounts.dat \
        -o compiler/include/pyc/rt/capi_table.hpp

Derived, not hand-written. Hand-maintaining refcount contracts for 800+
functions is exactly the error-prone bookkeeping that produces leaks, and
CPython already ships the answer in Doc/data/refcounts.dat.

WHAT THAT FILE IS AUTHORITATIVE FOR: return-value ownership. `+1` is a new
reference (Owned), `0` is borrowed, `null` means the function always returns
NULL, and a blank field means the return is not a PyObject*.

WHAT IT IS NOT: reference *stealing* by parameters. Its own header says so:

    XXX NOTE: the 0/+1/-1 refcount information for arguments is confusing!
    Much more useful would be to indicate whether the function "steals" a
    reference to the argument or not. Take for example PyList_SetItem(list,
    i, item). This lists as a 0 change for both the list and the item
    arguments. However, in fact it steals a reference to the item argument!

Verified: PyList_SetItem's `item` parameter really does record `0`, which is
indistinguishable from an ordinary borrow. Stealing is therefore a CURATED
list below, and must be reviewed when the target CPython moves. Getting it
wrong leaks (if we INCREF a stolen ref) or double-frees (if we DECREF one).
"""

from __future__ import annotations

import argparse
import collections
import sys

# Parameters that STEAL a reference. Not derivable from refcounts.dat.
# Keep conservative: a function absent here is treated as non-stealing, which
# is the safe direction (an extra DECREF by us is a leak, not a crash), and
# every entry should be justifiable from the C-API docs.
# Parameter names must match refcounts.dat EXACTLY -- they are the doc's
# names, not the source's, and they are not uniform (PyList_SetItem calls it
# `item` while PyTuple_SetItem calls the same thing `o`). A name that does not
# match is a hard error below, because a silently unapplied steal annotation
# means lowering emits a DECREF on a stolen reference: a double free.
_STEALS = {
    "PyList_SetItem":            ["item"],
    "PyTuple_SetItem":           ["o"],
    "PyStructSequence_SetItem":  ["o"],
    "PyErr_Restore":             ["type", "value", "traceback"],
    "PyException_SetCause":      ["cause"],
    "PyException_SetContext":    ["ctx"],
}

# Symbols lowering must NOT emit, with the reason and the replacement.
# A contract that cannot be expressed statically is a contract we refuse.
_BANNED = {
    "PyModule_AddObject": (
        "steals a reference ONLY on success, so the caller's cleanup path "
        "differs by outcome and cannot be expressed as a single static "
        "contract; use PyModule_AddObjectRef, which never steals"),
    "PyEval_CallObject": ("removed/deprecated; use PyObject_CallObject"),
    "PyEval_CallFunction": ("removed/deprecated; use PyObject_CallFunction"),
}


def parse(path: str) -> dict:
    funcs: dict[str, dict] = collections.OrderedDict()
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split(":")
        if len(parts) < 4:
            continue
        fn, typ, name, rc = parts[0], parts[1], parts[2], parts[3]
        f = funcs.setdefault(fn, {"ret": None, "params": []})
        if name == "":
            f["ret"] = (typ, rc)
        else:
            f["params"].append({"type": typ, "name": name, "rc": rc})
    return funcs


def ownership(typ: str, rc: str) -> str:
    if not typ.startswith("PyObject") and not typ.startswith("PyTypeObject"):
        return "NotAnObject"
    if rc == "+1":   return "Owned"
    if rc == "0":    return "Borrowed"
    if rc == "null": return "AlwaysNull"
    return "NotAnObject"


def may_raise(typ: str, rc: str) -> bool:
    """Conservative: assume a call can fail unless it plainly cannot.

    Being wrong in this direction costs a redundant error edge, which A3 can
    optimise away later. Being wrong the other way loses an exception, which
    is a silent wrong answer (I1) -- so `void` is the only thing treated as
    infallible.
    """
    return typ.strip() not in ("void", "")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("refcounts_dat")
    ap.add_argument("-o", "--out", default="-")
    ap.add_argument("--source-version", default="unknown")
    args = ap.parse_args()

    funcs = parse(args.refcounts_dat)

    # Validate the curated lists against the data. An unmatched steal
    # annotation is FATAL: it fails open, and failing open here means a
    # DECREF on a stolen reference.
    problems: list[str] = []
    for name, params in _STEALS.items():
        if name not in funcs:
            problems.append(f"_STEALS names {name!r}, absent from refcounts.dat")
            continue
        actual = [p["name"] for p in funcs[name]["params"]]
        for pn in params:
            if pn not in actual:
                problems.append(
                    f"_STEALS[{name!r}] names parameter {pn!r}, but the "
                    f"parameters are {actual}")
    if problems:
        print("gen_capi_table: curated steal list does not match the data:",
              file=sys.stderr)
        for pr in problems:
            print(f"  {pr}", file=sys.stderr)
        print("\nRefusing to generate. An unapplied steal annotation makes "
              "lowering emit a\nDECREF on a stolen reference, which is a "
              "double free.", file=sys.stderr)
        return 2
    for name in _BANNED:
        if name not in funcs:
            print(f"gen_capi_table: note: banned symbol {name!r} is absent "
                  f"from refcounts.dat; the ban is inert", file=sys.stderr)

    o: list[str] = []
    w = o.append
    w("#pragma once")
    w("// GENERATED by compiler/tools/gen_capi_table.py -- DO NOT EDIT.")
    w(f"// Source: CPython {args.source_version} Doc/data/refcounts.dat")
    w("//")
    w("// Return ownership is authoritative (CPython's own data). Parameter")
    w("// STEALING is a curated list in the generator: refcounts.dat records")
    w("// PyList_SetItem's stolen `item` as '0', indistinguishable from a")
    w("// borrow, and its header says so explicitly.")
    w('#include "pyc/rt/capi.hpp"')
    w("")
    w("namespace pyc::rt {")
    w("")
    w("inline constexpr CApiSymbol kCApiSymbols[] = {")
    n_owned = n_bor = n_steal = n_banned = 0
    for name, f in funcs.items():
        if not f["ret"]:
            continue
        typ, rc = f["ret"]
        own = ownership(typ, rc)
        n_owned += own == "Owned"
        n_bor += own == "Borrowed"
        steals = _STEALS.get(name, [])
        idx = [i for i, p in enumerate(f["params"]) if p["name"] in steals]
        n_steal += bool(idx)
        banned = _BANNED.get(name)
        n_banned += bool(banned)
        steal_init = "{" + ",".join(str(i) for i in idx) + "}" if idx else "{}"
        w(f'    {{"{name}", Ownership::{own}, '
          f'{"true" if may_raise(typ, rc) else "false"}, '
          f'{len(f["params"])}, {steal_init}, '
          f'{"true" if banned else "false"}, '
          f'"{banned if banned else ""}"}},')
    w("};")
    w("")
    w(f"inline constexpr int kCApiSymbolCount = "
      f"{sum(1 for f in funcs.values() if f['ret'])};")
    w("")
    w("}  // namespace pyc::rt")

    text = "\n".join(o) + "\n"
    if args.out == "-":
        sys.stdout.write(text)
    else:
        open(args.out, "w").write(text)
        print(f"{len(funcs)} symbols -> {args.out}")
        print(f"  Owned {n_owned}  Borrowed {n_bor}  "
              f"steal-annotated {n_steal}  banned {n_banned}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
