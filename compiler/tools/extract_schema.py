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

# Deprecated aliases that shadow real nodes; not part of the grammar.
_ALIASES = {"Num", "Str", "Bytes", "NameConstant", "Ellipsis", "Index", "ExtSlice"}


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
    concrete = [c for c in nodes if not c.__subclasses__()]
    sums = {c.__name__: sorted(s.__name__ for s in c.__subclasses__()
                               if s.__name__ not in _ALIASES)
            for c in nodes if c.__subclasses__()}

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
            fields.append({"name": f, "type": tname, "quant": quant})
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
