"""ast -> JSON. Total by construction.

Every node is encoded from `_fields` and `_attributes` reflectively, so a node
kind added by a future CPython is carried across without this file changing.
Nothing is special-cased per node type -- that is what made the old tree's
converter lossy (it hand-listed fields and silently dropped the rest).

Constant values need typed encoding because JSON cannot represent Python's
literals faithfully: int is arbitrary precision (a JSON number would lose
digits above 2**53), float has inf/nan, and bytes are not text.
"""

from __future__ import annotations

import ast
import base64


def _has_surrogates(s: str) -> bool:
    return any(0xD800 <= ord(c) <= 0xDFFF for c in s)


def _enc_str(v: str):
    """Python str is a sequence of CODE POINTS and may hold lone surrogates.

    JSON cannot carry those: it escapes astral characters *as* surrogate
    pairs, so a decoder silently recombines two surrogate code points into one
    astral character -- 2 in, 1 out. CPython's own Lib/test/datetimetester.py
    contains lone surrogates, so this is not hypothetical.

    Strings that contain surrogates therefore travel as base64 of
    utf-8/surrogatepass. Everything else stays readable text.
    """
    if _has_surrogates(v):
        return {"t": "str_raw",
                "v": base64.b64encode(v.encode("utf-8", "surrogatepass")).decode("ascii")}
    return {"t": "str", "v": v}


def _enc_constant(v):
    # Order matters: bool before int (bool is a subclass), and None/Ellipsis
    # are singletons rather than values.
    if v is None:
        return {"t": "none"}
    if v is Ellipsis:
        return {"t": "ellipsis"}
    if isinstance(v, bool):
        return {"t": "bool", "v": v}
    if isinstance(v, int):
        return {"t": "int", "v": str(v)}          # str: arbitrary precision
    if isinstance(v, float):
        return {"t": "float", "v": repr(v)}       # repr: inf/nan/round-trip
    if isinstance(v, complex):
        return {"t": "complex", "re": repr(v.real), "im": repr(v.imag)}
    if isinstance(v, str):
        return _enc_str(v)
    if isinstance(v, bytes):
        return {"t": "bytes", "v": base64.b64encode(v).decode("ascii")}
    if isinstance(v, tuple):
        return {"t": "tuple", "v": [_enc_constant(x) for x in v]}
    if isinstance(v, frozenset):
        return {"t": "frozenset", "v": [_enc_constant(x) for x in v]}
    raise TypeError(f"unencodable constant of type {type(v).__name__!r}")


def _enc_value(v):
    if isinstance(v, ast.AST):
        return encode_node(v)
    if isinstance(v, list):
        return [_enc_value(x) for x in v]
    # Bare field values: identifiers, docstrings, int flags (level, conversion,
    # is_async), and None for optional fields.
    if isinstance(v, str) and _has_surrogates(v):
        return {"_str_raw": base64.b64encode(
            v.encode("utf-8", "surrogatepass")).decode("ascii")}
    if v is None or isinstance(v, (str, int, float, bool)):
        return v
    if isinstance(v, bytes):
        return {"_bytes": base64.b64encode(v).decode("ascii")}
    raise TypeError(f"unencodable field value of type {type(v).__name__!r}")


def encode_node(node: ast.AST) -> dict:
    out = {"_kind": type(node).__name__}
    for f in node._fields:
        # A missing optional field is not the same as a field set to None;
        # ast omits some entirely. Preserve that distinction.
        if not hasattr(node, f):
            continue
        v = getattr(node, f)
        out[f] = _enc_constant(v) if (f == "value" and isinstance(node, ast.Constant)) \
            else _enc_value(v)
    for a in node._attributes:
        if hasattr(node, a):
            av = getattr(node, a)
            if av is not None:
                out[a] = av
    return out
