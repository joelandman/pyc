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
import re
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
    # Documented as stealing: "This call steals a reference to exc." Added
    # when try/except lowering needed it -- the list grows as symbols are
    # used, and an unlisted stealer means a decref on a reference we no
    # longer own.
    "PyErr_SetRaisedException":  ["exc"],
}

# Ownership for symbols no source records. stable_abi.toml lists them but
# carries no refcount data, so they arrive Unknown and are not emittable.
# Curate ONE AT A TIME, with the C-API docs as justification -- a wrong entry
# here leaks or double-frees, and there are 226 candidates, so bulk-guessing
# would be the worst possible trade.
#
# Format: name -> (return type as refcounts.dat would spell it, refcount field)
_OWNERSHIP_OVERRIDES = {
    # Returns int (0 / -1), holds its own reference, never steals. It exists
    # precisely because PyModule_AddObject's success-only steal is unsafe, so
    # it is the replacement _BANNED points at and must be usable.
    "PyModule_AddObjectRef": ("int", "", ["PyObject*", "const char*", "PyObject*"]),
    # Documented as returning a new reference to the exception currently being
    # handled, or NULL when there is none. Needed for bare `raise`.
    "PyErr_GetHandledException": ("PyObject*", "+1", []),
    # Returns a new reference to the formatted string. format_spec may be NULL,
    # which means "no spec" -- not the same as an empty one for a type with a
    # custom __format__.
    "PyObject_Format": ("PyObject*", "+1", ["PyObject*", "PyObject*"]),
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


def parse_stable_abi(path: str) -> dict:
    """Misc/stable_abi.toml -> {name: {"added": "3.x"}}.

    A second, independent source. It records which symbols exist and are
    stable-ABI, and WHEN each was added -- but carries no ownership data. So it
    widens coverage without answering the refcount question: a symbol present
    only here is marked Ownership::Unknown and is not emittable.

    Parsed with a regex rather than tomllib so the generator runs under the
    host python regardless of version.
    """
    out: dict[str, dict] = {}
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\[function\.([A-Za-z_][A-Za-z0-9_]*)\]", line.strip())
        if m:
            cur = m.group(1)
            out[cur] = {}
            continue
        if line.startswith("["):
            cur = None
            continue
        if cur and line.startswith((" ", "\t")):
            m2 = re.match(r"(\w+)\s*=\s*'([^']*)'", line.strip())
            if m2:
                out[cur][m2.group(1)] = m2.group(2)
    return out


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


def param_kind(typ: str) -> str:
    """One char per parameter: how codegen must pass it.

    Assuming every argument is a PyObject* is wrong and fails loudly at
    assembly time -- PyBool_FromLong takes a long, and emitting `ptr` for it
    is an LLVM type error. Recording the kind makes the calling convention
    come from CPython's data rather than from an assumption.

      o  PyObject*-like        (ptr)
      p  raw pointer / string  (ptr)
      i  integer               (i64)
      d  double
      ?  unrecognised -- codegen refuses rather than guessing
    """
    t = typ.strip().rstrip("*").strip()
    stars = typ.count("*")
    if typ.strip() in ("PyObject*", "PyTypeObject*", "PyVarObject*",
                       "PyCodeObject*", "PyFrameObject*", "PyFunctionObject*"):
        return "o"
    if stars:
        return "p"
    if t in ("int", "long", "unsigned int", "unsigned long", "Py_ssize_t",
             "size_t", "Py_UCS4", "Py_hash_t", "char", "unsigned char",
             "long long", "unsigned long long", "Py_uintptr_t", "uint64_t",
             "int64_t", "Py_off_t"):
        return "i"
    if t in ("double", "float"):
        return "d"
    return "?"


def ownership(typ: str, rc: str) -> str:
    if rc == "unknown":
        return "Unknown"
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


# Helpers pyc itself provides. They live in the same table so lowering asks
# exactly one question -- "is this emittable?" -- regardless of who implements
# the symbol.
_PYC_RUNTIME = {
    # name: (return type, refcount, [param types])
    "pyc_rt_bind_method": ("PyObject*", "+1", ["PyObject*"]),
    "pyc_rt_cm_exit":      ("PyObject*", "+1", ["PyObject*"]),
    "pyc_rt_cm_enter":     ("PyObject*", "+1", ["PyObject*"]),
    "pyc_rt_exit_normal":  ("int", "", ["PyObject*"]),
    "pyc_rt_exit_exc":     ("int", "", ["PyObject*"]),
    "pyc_rt_extend":       ("int", "", ["PyObject*", "PyObject*"]),
    "pyc_rt_assert_fail":  ("int", "", ["PyObject*"]),
    "pyc_rt_del_global":   ("int", "", ["const char*"]),
    "pyc_rt_reraise":      ("int", "", []),
    "pyc_rt_super_fail":   ("int", "", ["int"]),
    "pyc_rt_match_sequence": ("PyObject*", "+1",
                              ["PyObject*", "Py_ssize_t", "Py_ssize_t", "int"]),
    "pyc_rt_match_mapping":  ("PyObject*", "+1", ["PyObject*", "PyObject*", "int"]),
    "pyc_rt_match_class":    ("PyObject*", "+1",
                              ["PyObject*", "PyObject*", "int", "PyObject*"]),
    "pyc_rt_push_handled": ("PyObject*", "+1", ["PyObject*"]),
    "pyc_rt_pop_handled":  ("int", "", ["PyObject*"]),
    "pyc_rt_import_star":  ("int", "", ["PyObject*"]),
    "pyc_rt_cell_get":     ("PyObject*", "+1", ["PyObject*"]),
    "pyc_rt_unpack_ex":    ("PyObject*", "+1", ["PyObject*", "Py_ssize_t", "Py_ssize_t"]),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("refcounts_dat")
    ap.add_argument("--stable-abi", default=None,
                    help="Misc/stable_abi.toml, for symbols refcounts.dat omits")
    ap.add_argument("-o", "--out", default="-")
    ap.add_argument("--source-version", default="unknown")
    args = ap.parse_args()

    funcs = parse(args.refcounts_dat)
    stable = parse_stable_abi(args.stable_abi) if args.stable_abi else {}
    for name, (rt, rc, ptypes) in _PYC_RUNTIME.items():
        funcs[name] = {"ret": (rt, rc),
                       "params": [{"type": t, "name": f"a{i}", "rc": ""}
                                  for i, t in enumerate(ptypes)]}

    # Symbols the stable ABI has but refcounts.dat does not. Included so the
    # gap is enumerable rather than invisible, but with ownership Unknown, so
    # lowering still cannot emit them (INTERFACES §4).
    for name, meta in stable.items():
        if name not in funcs:
            funcs[name] = {"ret": ("<unknown>", "unknown"), "params": [],
                           "_stable_only": True, "_added": meta.get("added", "")}

    # Apply curated ownership, and validate it. An override for a symbol that
    # refcounts.dat already describes is a conflict, not a refinement: one of
    # the two is wrong and silently preferring either is how contradictions
    # get baked in.
    for name, (typ, rc, ptypes) in _OWNERSHIP_OVERRIDES.items():
        if name not in funcs:
            print(f"gen_capi_table: WARNING: ownership override for {name!r}, "
                  f"which no source lists at all", file=sys.stderr)
            continue
        if not funcs[name].get("_stable_only"):
            print(f"gen_capi_table: ownership override for {name!r} conflicts "
                  f"with refcounts.dat, which already records it",
                  file=sys.stderr)
            return 2
        funcs[name]["ret"] = (typ, rc)
        funcs[name]["params"] = [{"type": t, "name": f"a{i}", "rc": ""}
                                 for i, t in enumerate(ptypes)]

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
    n_owned = n_bor = n_steal = n_banned = n_unknown = 0
    for name, f in funcs.items():
        if not f["ret"]:
            continue
        typ, rc = f["ret"]
        meta = stable.get(name, {})
        added = f.get("_added", meta.get("added", ""))
        own = ownership(typ, rc)
        n_owned += own == "Owned"
        n_bor += own == "Borrowed"
        n_unknown += own == "Unknown"
        steals = _STEALS.get(name, [])
        idx = [i for i, p in enumerate(f["params"]) if p["name"] in steals]
        n_steal += bool(idx)
        banned = _BANNED.get(name)
        n_banned += bool(banned)
        steal_init = "{" + ",".join(str(i) for i in idx) + "}" if idx else "{}"
        kinds = "".join(param_kind(pp["type"]) for pp in f["params"])
        w(f'    {{"{name}", Ownership::{own}, '
          f'{"true" if may_raise(typ, rc) else "false"}, '
          f'{len(f["params"])}, {steal_init}, '
          f'{"true" if banned else "false"}, '
          f'"{banned if banned else ""}", '
          f'{"true" if name in stable else "false"}, '
          f'"{added}", "{kinds}"}},')
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
        print(f"  stable-ABI only, ownership UNKNOWN (not emittable): {n_unknown}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
