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
        code, frees = _compile_one(node, frees, filename)   # co_freevars order
        out.append({
            "line": node.lineno,
            "col": node.col_offset,
            "freevars": frees,
            "code": base64.b64encode(code).decode("ascii"),
        })
    return out


def _compile_one(node: ast.GeneratorExp, frees: list[str],
                 filename: str) -> tuple[bytes, list[str]]:
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
    # co_freevars is AUTHORITATIVE and its order matters: the run side builds
    # the closure tuple positionally, so position i must be the cell for
    # co_freevars[i]. symtable.get_frees() returns FIRST-APPEARANCE order while
    # co_freevars is always sorted, so the two disagree whenever the free
    # variables are not referenced alphabetically -- `strides[i] * shape[i]`
    # gives ('strides','shape') from symtable and ('shape','strides') here.
    #
    # This used to be an equality assertion, which refused 19 Lib/test files.
    # The assertion was right to exist: without it the closure tuple would have
    # been built in symtable order and indexed in sorted order, silently binding
    # one variable's cell where another was expected. But the fix is to take the
    # authoritative order, not to demand symtable produce it.
    if set(inner.co_freevars) != set(frees):
        raise GenexpError(
            f"genexp freevars {inner.co_freevars} are not the same SET as "
            f"symtable {tuple(frees)}")
    return marshal.dumps(inner), list(inner.co_freevars)


def _only_code(consts, what: str) -> types.CodeType:
    codes = [c for c in consts if isinstance(c, types.CodeType)]
    if len(codes) != 1:
        raise GenexpError(f"expected exactly one {what} code object, found {len(codes)}")
    return codes[0]


# --- generator functions ---------------------------------------------------
#
# A def containing yield is a generator function, and needs the same treatment
# for the same reason: pyc has no suspension, so the body is compiled here and
# run by the linked interpreter. Unlike a genexp it has real parameters, so
# what pyc supplies is the closure cells and the DEFAULTS -- those are
# evaluated in the enclosing scope at def time, which is pyc's job, not the
# wrapper's.


def _is_generator_body(node) -> bool:
    """Does this def's own body yield? Nested scopes do not count."""
    SCOPES = (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)
    found = False

    def walk(n):
        nonlocal found
        if found:
            return
        for ch in ast.iter_child_nodes(n):
            # A nested scope has its own yields; `def outer` containing
            # `def inner` that yields is NOT itself a generator. The skip has
            # to apply to the statement itself, not only to its children.
            if isinstance(ch, SCOPES):
                continue
            if isinstance(ch, (ast.Yield, ast.YieldFrom)):
                found = True
                return
            walk(ch)

    for st in node.body:
        if isinstance(st, SCOPES):
            continue
        if isinstance(st, (ast.Yield, ast.YieldFrom)):
            return True
        walk(st)
    return found


def _qualname_of(stack: list[str], name: str) -> str:
    return ".".join(stack + [name]) if stack else name


def collect_genfuncs(tree: ast.Module, source: bytes, filename: str) -> list[dict]:
    """One entry per generator FUNCTION, keyed by source position."""
    out: list[dict] = []
    st = symtable.symtable(source, filename, "exec")

    def scope_for(scopes, name, lineno):
        for sc in scopes:
            if sc.get_name() == name and sc.get_lineno() == lineno:
                return sc
        return None

    def visit(node, scope, stack):
        children = scope.get_children() if scope else []
        for ch in ast.iter_child_nodes(node):
            if isinstance(ch, (ast.FunctionDef, ast.AsyncFunctionDef)):
                sub = scope_for(children, ch.name, ch.lineno)
                # An `async def` ALWAYS goes to CPython, whether it is a
                # coroutine or an async generator. await, async for and async
                # with are all SyntaxErrors outside an async function, so
                # compiling the body covers every one of them at once.
                if isinstance(ch, ast.AsyncFunctionDef):
                    if sub is None:
                        raise GenexpError(
                            f"no symtable scope for async function "
                            f"'{ch.name}' at line {ch.lineno}")
                    frees = list(sub.get_frees())
                    code, frees = _compile_genfunc(
                        ch, frees, _qualname_of(stack, ch.name),
                        filename, is_async=True)                       # co_freevars order
                    out.append({
                        "line": ch.lineno, "col": ch.col_offset,
                        "freevars": frees,
                        "code": base64.b64encode(code).decode("ascii"),
                    })
                elif isinstance(ch, ast.FunctionDef) and _is_generator_body(ch):
                    if sub is None:
                        raise GenexpError(
                            f"no symtable scope for generator function "
                            f"'{ch.name}' at line {ch.lineno}")
                    frees = list(sub.get_frees())
                    code, frees = _compile_genfunc(
                        ch, frees, _qualname_of(stack, ch.name),
                        filename)                       # co_freevars order
                    out.append({
                        "line": ch.lineno, "col": ch.col_offset,
                        "freevars": frees,
                        "code": base64.b64encode(code).decode("ascii"),
                    })
                visit(ch, sub, stack + [ch.name, "<locals>"])
            elif isinstance(ch, ast.ClassDef):
                sub = scope_for(children, ch.name, ch.lineno)
                visit(ch, sub, stack + [ch.name])
            else:
                visit(ch, scope, stack)

    visit(tree, st, [])
    return out


# CPython code-object flags. A body compiled here must come back as the kind
# of function it was written as; asserting that catches a detection bug at
# BUILD time rather than shipping a binary that quietly does the wrong thing.
CO_GENERATOR = 0x20
CO_COROUTINE = 0x80
CO_ASYNC_GENERATOR = 0x200


def _compile_genfunc(node, frees: list[str], qualname: str, filename: str,
                     is_async: bool = False) -> tuple[bytes, list[str]]:
    shim = copy.deepcopy(node)
    # Decorators, defaults and annotations are all evaluated by the ENCLOSING
    # scope at def time. Compiling them into the wrapper would evaluate them in
    # the wrong scope, at the wrong moment, or both. pyc supplies the defaults
    # through the function object and applies the decorators itself.
    shim.decorator_list = []
    shim.returns = None
    shim.args.defaults = []
    shim.args.kw_defaults = [None] * len(shim.args.kw_defaults)
    for a in (list(shim.args.args) + list(shim.args.posonlyargs)
              + list(shim.args.kwonlyargs)):
        a.annotation = None
    if shim.args.vararg: shim.args.vararg.annotation = None
    if shim.args.kwarg: shim.args.kwarg.annotation = None
    ast.fix_missing_locations(shim)

    params = [ast.arg(arg=f) for f in frees]
    wrapper = ast.Module(
        body=[ast.FunctionDef(
            name="_pyc_wrap",
            args=ast.arguments(posonlyargs=[], args=params, vararg=None,
                               kwonlyargs=[], kw_defaults=[], kwarg=None,
                               defaults=[]),
            body=[shim],
            decorator_list=[], returns=None, type_params=[])],
        type_ignores=[])
    ast.fix_missing_locations(wrapper)

    mod = compile(wrapper, filename, "exec")
    wrap_code = _only_code(mod.co_consts, "wrapper")
    inner = _only_code(wrap_code.co_consts, "generator function")
    if is_async:
        # Either a coroutine or an async generator, depending on whether the
        # body yields. Both are handed to the interpreter the same way.
        if not (inner.co_flags & (CO_COROUTINE | CO_ASYNC_GENERATOR)):
            raise GenexpError(
                f"'{node.name}' was not compiled as a coroutine or async "
                f"generator")
    elif not (inner.co_flags & CO_GENERATOR):
        raise GenexpError(f"'{node.name}' was not compiled as a generator")
    # Same as the genexp path: co_freevars order is authoritative because the
    # closure tuple is built positionally against it.
    if set(inner.co_freevars) != set(frees):
        raise GenexpError(
            f"'{node.name}' freevars {inner.co_freevars} are not the same SET "
            f"as symtable {tuple(frees)}")
    # co_qualname would read "_pyc_wrap.<locals>.name"; correct it at build
    # time so tracebacks and repr name the real function.
    return marshal.dumps(inner.replace(co_qualname=qualname)), list(inner.co_freevars)
