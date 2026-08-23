#!/usr/bin/env python3
"""Extract a normalized AST schema from the TARGET interpreter.

Run by the target python, not the host:
    <sysroot>/bin/python3.X compiler/tools/extract_schema.py -o schema.json

Prefers `_field_types` (CPython 3.13+), which carries the complete typed
schema reflectively -- field types, optionality and sequence-ness -- so no
CPython source tarball is needed. Falls back to `_fields` alone on <=3.12,
which loses type information and is therefore reported as degraded.
"""

from __future__ import annotations

import argparse
import ast
import json
import sys
import types
import typing

# Names CPython keeps for backwards compatibility that are NOT part of the
# current grammar. There is no programmatic marker for these, so the list is
# curated and MUST be reviewed each time the target version moves.
#
# `slice` is the vestigial abstract base of the slice sum that Python 3.9
# removed; its only subclasses are Index/ExtSlice, themselves removed. The
# Num/Str/Bytes/NameConstant/Ellipsis aliases persist through 3.13 as
# subclasses of Constant and are gone in 3.14 -- which is exactly why they
# must be filtered *consistently*, including when asking whether a class has
# subclasses at all. Counting them made Constant look abstract on 3.13 and
# silently dropped it from the schema entirely.
_ALIASES = {"Num", "Str", "Bytes", "NameConstant", "Ellipsis",
            "Index", "ExtSlice", "slice"}

# Fields whose LIST may contain None, which `_field_types` does not express:
# it declares Dict.keys as list[ast.expr], yet CPython stores None there to
# mark a `**` unpacking entry.
#
# Derived empirically, not guessed -- a scan of 14,641 stdlib + site-packages
# files found exactly these two (kw_defaults 1729 occurrences, Dict.keys 526):
#
#   for n in ast.walk(tree):
#       for f in n._fields:
#           v = getattr(n, f, None)
#           if isinstance(v, list) and any(x is None for x in v): ...
#
# Re-run that scan when the target version moves. An unlisted case is safe in
# the sense that matters: the deserializer rejects the unexpected null loudly
# with a diagnostic (I1) rather than dropping the element.
_NULLABLE_ELEMENTS = {"arguments.kw_defaults", "Dict.keys"}

# `_field_types` reports `object` for anything it cannot type, and that covers
# three different things. Constant.value and MatchSingleton.value hold Python
# LITERALS; Interpolation.str (PEP 750) is an ordinary str holding the source
# text of the interpolation. Collapsing them all to "a literal" makes both
# t-strings and `case None:` fail to deserialize.
#
# Curated deliberately: an `object` field NOT listed here keeps type "object",
# and gen_ast.py then refuses to generate, naming the field. Guessing is how
# this bug happened in the first place.
_OBJECT_FIELDS = {
    "Constant.value":       "constant",
    "MatchSingleton.value": "constant",
    "Interpolation.str":    "str",
}


def _subclasses(c: type) -> list[type]:
    """Real subclasses only.

    CPython keeps deprecated aliases (Num, Str, Bytes, ...) as subclasses of
    Constant through 3.13 and removes them in 3.14. Counting them makes
    Constant look ABSTRACT on 3.13, which silently dropped it from the schema
    entirely -- a header with no Constant node. Filter consistently everywhere
    that asks "does this have subclasses".
    """
    return [x for x in c.__subclasses__() if x.__name__ not in _ALIASES]


def _all_nodes() -> list[type]:
    out, seen = [], set()
    stack = [ast.AST]
    while stack:
        c = stack.pop()
        for sub in c.__subclasses__():
            if sub.__name__ in seen:
                continue
            seen.add(sub.__name__)
            out.append(sub)
            stack.append(sub)
    return out


def _norm_type(t) -> tuple[str, str]:
    """-> (type_name, quant) where quant in {one, opt, seq}."""
    origin = typing.get_origin(t)
    if origin is list:
        (inner,) = typing.get_args(t)
        name, _ = _norm_type(inner)
        return name, "seq"
    if origin is types.UnionType or origin is typing.Union:
        args = [a for a in typing.get_args(t) if a is not type(None)]
        if len(args) == 1:
            name, q = _norm_type(args[0])
            return name, "opt" if q == "one" else q
        return "constant", "one"        # e.g. Constant.value's wide union
    if isinstance(t, type):
        return t.__name__, "one"
    return str(t), "one"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default="-")
    args = ap.parse_args()

    nodes = [c for c in _all_nodes() if c.__name__ not in _ALIASES]
    by_name = {c.__name__: c for c in nodes}
    concrete = [c for c in nodes if not _subclasses(c)]
    sums = {c.__name__: sorted(x.__name__ for x in _subclasses(c))
            for c in nodes if _subclasses(c)}

    degraded = not hasattr(ast.FunctionDef, "_field_types")
    out_nodes = {}
    for c in concrete:
        ft = getattr(c, "_field_types", None)
        fields = []
        for f in c._fields:
            if ft is not None and f in ft:
                tname, quant = _norm_type(ft[f])
            else:
                tname, quant = ("unknown", "one")
            if tname == "object":
                tname = _OBJECT_FIELDS.get(f"{c.__name__}.{f}", "object")
            entry = {"name": f, "type": tname, "quant": quant}
            if f"{c.__name__}.{f}" in _NULLABLE_ELEMENTS:
                entry["nullable_elements"] = True
            fields.append(entry)
        base = next((b.__name__ for b in c.__mro__[1:]
                     if b is not ast.AST and b.__name__ in sums), None)
        out_nodes[c.__name__] = {
            "base": base,
            "fields": fields,
            "attributes": list(c._attributes),
        }

    # A sum is "trivial" when every constructor is field-free (expr_context,
    # boolop, operator, unaryop, cmpop). Those need no indirection in C++,
    # which keeps `ctx` on every Name node allocation-free.
    trivial = sorted(s for s, ctors in sums.items()
                     if ctors and all(not out_nodes.get(k, {}).get("fields")
                                      for k in ctors))

    schema = {
        "schema_version": 1,
        "python_version": "%d.%d.%d" % sys.version_info[:3],
        "degraded": degraded,
        "sums": {k: v for k, v in sorted(sums.items()) if v},
        "trivial_sums": trivial,
        "nullable_elements": sorted(_NULLABLE_ELEMENTS),
        "object_fields": dict(sorted(_OBJECT_FIELDS.items())),
        "nodes": dict(sorted(out_nodes.items())),
    }
    text = json.dumps(schema, indent=2) + "\n"
    if args.out == "-":
        sys.stdout.write(text)
    else:
        open(args.out, "w").write(text)
        print(f"{len(out_nodes)} concrete nodes, {len(schema['sums'])} sums "
              f"({len(trivial)} trivial) -> {args.out}"
              + ("  [DEGRADED: no _field_types]" if degraded else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
