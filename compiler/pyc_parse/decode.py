"""JSON -> ast. The inverse of encode.py, and the reference semantics for the
C++ deserializer (INTERFACES.md §2.1).

Exists primarily to make the round-trip provable: encode -> decode -> ast.dump
must equal the original dump, over all of CPython's Lib/. A field dropped
anywhere shows up as a mismatch.
"""

from __future__ import annotations

import ast
import base64


_LITERAL_FIELDS = {("Constant", "value"), ("MatchSingleton", "value")}


def _dec_constant(d):
    t = d["t"]
    if t == "none":      return None
    if t == "ellipsis":  return Ellipsis
    if t == "bool":      return d["v"]
    if t == "int":       return int(d["v"])
    if t == "float":     return float(d["v"])
    if t == "complex":   return complex(float(d["re"]), float(d["im"]))
    if t == "str":       return d["v"]
    if t == "str_raw":   return base64.b64decode(d["v"]).decode("utf-8", "surrogatepass")
    if t == "bytes":     return base64.b64decode(d["v"])
    if t == "tuple":     return tuple(_dec_constant(x) for x in d["v"])
    if t == "frozenset": return frozenset(_dec_constant(x) for x in d["v"])
    raise ValueError(f"unknown constant tag {t!r}")


def _dec_value(v):
    if isinstance(v, list):
        return [_dec_value(x) for x in v]
    if isinstance(v, dict):
        if "_kind" in v:
            return decode_node(v)
        if "_bytes" in v:
            return base64.b64decode(v["_bytes"])
        if "_str_raw" in v:
            return base64.b64decode(v["_str_raw"]).decode("utf-8", "surrogatepass")
        raise ValueError(f"unknown object in field position: {sorted(v)!r}")
    return v


def decode_node(d: dict) -> ast.AST:
    kind = d["_kind"]
    cls = getattr(ast, kind, None)
    if cls is None or not (isinstance(cls, type) and issubclass(cls, ast.AST)):
        # Loud, per CHARTER I1: a node this interpreter does not know is a hard
        # error naming the construct, never a silent skip.
        raise ValueError(f"unknown AST node kind {kind!r} for this interpreter")
    node = cls()
    for f in cls._fields:
        if f not in d:
            continue
        v = d[f]
        setattr(node, f, _dec_constant(v) if (kind, f) in _LITERAL_FIELDS
                else _dec_value(v))
    for a in cls._attributes:
        if a in d:
            setattr(node, a, d[a])
    return node
