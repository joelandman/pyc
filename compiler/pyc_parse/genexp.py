"""Generator expressions, compiled by CPython at build time.

See rebuild/GENERATORS.md. pyc does not implement suspension: each generator
expression is compiled here into a code object, marshalled, and re-created at
run time by the interpreter the binary already links. pyc supplies the two
things that code object needs from its enclosing scope -- the eagerly
evaluated outer iterator, and a cell per free variable.

The free-variable list comes from `symtable`, which is CPython's own scope
analysis. It matters that pyc does not re-derive it: symtable distinguishes an
enclosing function's local (free, needs a cell) from a global (resolved
through __globals__, no cell), and an over-approximating list would turn a
global into a free variable and demand a cell that cannot exist.
"""

from __future__ import annotations

import ast
import base64
import copy
import marshal
import symtable
import types

# The genexp code object's first argument is the outer ITERATOR, named `.0` by
# CPython. In the wrapper below it needs a name that is a legal identifier and
# cannot collide with user code.
_ITER_PARAM = "_pyc_outer_iter"


class GenexpError(Exception):
    pass


def _genexp_scopes(st, out):
    for ch in st.get_children():
        if ch.get_name() == "genexpr":
            out.append(ch)
        _genexp_scopes(ch, out)


def collect(tree: ast.Module, source: bytes, filename: str) -> list[dict]:
    """One entry per generator expression, keyed by source position."""
    nodes = [n for n in ast.walk(tree) if isinstance(n, ast.GeneratorExp)]
    if not nodes:
        return []
    # ast.walk is breadth-first; sort into source order so both sides agree.
    nodes.sort(key=lambda n: (n.lineno, n.col_offset))

    scopes: list = []
    _genexp_scopes(symtable.symtable(source, filename, "exec"), scopes)
    scopes.sort(key=lambda s: s.get_lineno())

    if len(scopes) != len(nodes):
        raise GenexpError(
            f"{len(nodes)} generator expressions but {len(scopes)} symtable "
            f"scopes; refusing to guess which closure belongs to which")

    out = []
    for node, scope in zip(nodes, scopes):
        # A disagreement here would silently pair a genexp with another's
        # closure, so it is an error rather than a best effort.
        if scope.get_lineno() != node.lineno:
            raise GenexpError(
                f"generator expression at line {node.lineno} does not match "
                f"symtable scope at line {scope.get_lineno()}")
        frees = list(scope.get_frees())
        out.append({
            "line": node.lineno,
            "col": node.col_offset,
            "freevars": frees,
            "code": base64.b64encode(_compile_one(node, frees, filename)).decode("ascii"),
        })
    return out


def _compile_one(node: ast.GeneratorExp, frees: list[str], filename: str) -> bytes:
    # The outermost iterable is evaluated EAGERLY by the enclosing scope and
    # handed in, so inside the wrapper it is just a parameter.
    shim = copy.deepcopy(node)
    shim.generators[0].iter = ast.Name(id=_ITER_PARAM, ctx=ast.Load())
    ast.fix_missing_locations(shim)

    params = [ast.arg(arg=f) for f in frees]
    wrapper = ast.Module(
        body=[ast.FunctionDef(
            name="_pyc_wrap",
            args=ast.arguments(posonlyargs=[], args=params, vararg=None,
                               kwonlyargs=[], kw_defaults=[], kwarg=None,
                               defaults=[]),
            body=[ast.Return(value=shim)],
            decorator_list=[], returns=None, type_params=[])],
        type_ignores=[])
    ast.fix_missing_locations(wrapper)

    mod = compile(wrapper, filename, "exec")
    wrap_code = _only_code(mod.co_consts, "wrapper")
    inner = _only_code(wrap_code.co_consts, "generator expression")

    # The contract the run side depends on, checked here rather than trusted:
    # one argument (the iterator) and exactly the free variables symtable named.
    if inner.co_argcount != 1:
        raise GenexpError(f"genexp code takes {inner.co_argcount} args, expected 1")
    if list(inner.co_freevars) != frees:
        raise GenexpError(
            f"genexp freevars {inner.co_freevars} do not match symtable {tuple(frees)}")
    return marshal.dumps(inner)


def _only_code(consts, what: str) -> types.CodeType:
    codes = [c for c in consts if isinstance(c, types.CodeType)]
    if len(codes) != 1:
        raise GenexpError(f"expected exactly one {what} code object, found {len(codes)}")
    return codes[0]
