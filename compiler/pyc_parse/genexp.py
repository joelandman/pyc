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
        # User `def genexpr(...)` is also named genexpr. The compiler's
        # wrapper uses `.0` as the outer-iterator parameter; a method
        # named genexpr does not.
        if ch.get_name() == "genexpr":
            params = ch.get_parameters() if hasattr(ch, "get_parameters") else ()
            if ".0" in params:
                out.append(ch)
        _genexp_scopes(ch, out)


def _genexp_nodes(tree: ast.Module) -> list[tuple[ast.GeneratorExp, str, int]]:
    found: list[tuple[ast.GeneratorExp, str, int]] = []

    def visit(node, stack):
        for ch in ast.iter_child_nodes(node):
            if isinstance(ch, ast.GeneratorExp):
                found.append((ch, _qualname_of(stack, "<genexpr>"),
                              _enclosing_flags(stack)))
                visit(ch, stack)
            elif isinstance(ch, (ast.FunctionDef, ast.AsyncFunctionDef)):
                visit(ch, stack + [ch.name, "<locals>"])
            elif isinstance(ch, ast.ClassDef):
                visit(ch, stack + [ch.name])
            elif isinstance(ch, ast.Lambda):
                visit(ch, stack + ["<lambda>", "<locals>"])
            else:
                visit(ch, stack)

    visit(tree, [])
    return found


def collect(tree: ast.Module, source: bytes, filename: str) -> list[dict]:
    """One entry per generator expression, keyed by source position."""
    found = _genexp_nodes(tree)
    if not found:
        return []
    found.sort(key=lambda t: (t[0].lineno, t[0].col_offset))

    scopes: list = []
    _genexp_scopes(symtable.symtable(source, filename, "exec"), scopes)
    scopes.sort(key=lambda s: s.get_lineno())

    if len(scopes) != len(found):
        raise GenexpError(
            f"{len(found)} generator expressions but {len(scopes)} symtable "
            f"scopes; refusing to guess which closure belongs to which")

    out = []
    future_flags = _future_flags(tree)
    for (node, qualname, enclosing), scope in zip(found, scopes):
        # A disagreement here would silently pair a genexp with another's
        # closure, so it is an error rather than a best effort.
        if scope.get_lineno() != node.lineno:
            raise GenexpError(
                f"generator expression at line {node.lineno} does not match "
                f"symtable scope at line {scope.get_lineno()}")
        frees = list(scope.get_frees())
        code, frees = _compile_one(node, frees, filename, qualname,
                                   enclosing, future_flags)
        out.append({
            "line": node.lineno,
            "col": node.col_offset,
            "freevars": frees,
            "code": base64.b64encode(code).decode("ascii"),
        })
    return out


def _compile_one(node: ast.GeneratorExp, frees: list[str], filename: str,
                 qualname: str, enclosing_flags: int = 0,
                 future_flags: int = 0) -> tuple[bytes, list[str]]:
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

    mod = compile(wrapper, filename, "exec", flags=future_flags,
                  dont_inherit=True)
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
    # The wrapper compilation places the genexp inside `_pyc_wrap`, so its
    # CO_NESTED is the wrapper's, not the source's; and no source placement
    # ever yields METHOD inside a wrapper. Rewrite the scope bits with the
    # real enclosing context, restore the real qualname, and remap the
    # nested code objects' wrapper-derived qualnames.
    wrapper_qual = inner.co_qualname
    flags = (inner.co_flags & ~(_CO_NESTED | _CO_METHOD)) | enclosing_flags
    inner = inner.replace(co_qualname=qualname, co_flags=flags)
    inner = _remap_nested_qualnames(inner, wrapper_qual)
    return (marshal.dumps(inner), list(inner.co_freevars))


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


def _is_generator_expr(e) -> bool:
    """Does this expression yield in THIS scope? Nested lambdas do not count."""
    SCOPES = (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)
    if isinstance(e, (ast.Yield, ast.YieldFrom)):
        return True
    found = False

    def walk(n):
        nonlocal found
        if found:
            return
        for ch in ast.iter_child_nodes(n):
            if isinstance(ch, SCOPES):
                continue
            if isinstance(ch, (ast.Yield, ast.YieldFrom)):
                found = True
                return
            walk(ch)

    walk(e)
    return found


def _lambda_as_func(node: ast.Lambda) -> ast.FunctionDef:
    fn = ast.FunctionDef(
        name="<lambda>",
        args=copy.deepcopy(node.args),
        body=[ast.Return(value=copy.deepcopy(node.body))],
        decorator_list=[], returns=None, type_params=[],
        lineno=node.lineno, col_offset=node.col_offset,
        end_lineno=getattr(node, "end_lineno", None),
        end_col_offset=getattr(node, "end_col_offset", None))
    ast.fix_missing_locations(fn)
    return fn


def _qualname_of(stack: list[str], name: str) -> str:
    return ".".join(stack + [name]) if stack else name


def collect_genfuncs(tree: ast.Module, source: bytes, filename: str) -> list[dict]:
    """One entry per generator FUNCTION, keyed by source position."""
    out: list[dict] = []
    st = symtable.symtable(source, filename, "exec")
    future_flags = _future_flags(tree)

    def scope_for(scopes, name, lineno):
        for sc in scopes:
            if sc.get_name() == name and sc.get_lineno() == lineno:
                if sc.get_type() == "type parameters":
                    found = scope_for(sc.get_children(), name, lineno)
                    if found is not None:
                        return found
                    continue
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
                        filename, is_async=True,                       # co_freevars order
                        enclosing_flags=_enclosing_flags(stack),
                        future_flags=future_flags)
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
                        filename,                                      # co_freevars order
                        enclosing_flags=_enclosing_flags(stack),
                        future_flags=future_flags)
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
                if isinstance(ch, ast.Lambda):
                    sub = scope_for(children, "lambda", ch.lineno)
                    if _is_generator_expr(ch.body):
                        if sub is None:
                            raise GenexpError(
                                f"no symtable scope for lambda generator "
                                f"at line {ch.lineno}")
                        frees = list(sub.get_frees())
                        code, frees = _compile_genfunc(
                            _lambda_as_func(ch), frees,
                            _qualname_of(stack, "<lambda>"), filename,
                            enclosing_flags=_enclosing_flags(stack),
                            future_flags=future_flags)
                        out.append({
                            "line": ch.lineno, "col": ch.col_offset,
                            "freevars": frees,
                            "code": base64.b64encode(code).decode("ascii"),
                        })
                    visit(ch, sub, stack)
                    continue
                visit(ch, scope, stack)

    visit(tree, st, [])
    return out


# CPython code-object flags. A body compiled here must come back as the kind
# of function it was written as; asserting that catches a detection bug at
# BUILD time rather than shipping a binary that quietly does the wrong thing.
CO_GENERATOR = 0x20
CO_COROUTINE = 0x80
CO_ASYNC_GENERATOR = 0x200

# Scope-context bits (include/cpython/code.h). The wrapper compiles the body
# inside `_pyc_wrap`, so the CO_NESTED/CO_METHOD the wrapper compilation
# derives describe the WRONG placement and are corrected with the real
# enclosing context before the code object is shipped.
_CO_NESTED = 0x0010
_CO_METHOD = 0x8000000

# CO_FUTURE_* values (include/cpython/code.h). The wrapper is compiled
# separately from the user source, so the source's `from __future__`
# statements do not reach it the way they do for the real module -- and
# without dont_inherit, pyc_parse's own `from __future__ import annotations`
# leaks into the wrapper compilation and onto every shipped code object's
# co_flags (0x1000000 where CPython's own compilation has nothing). The
# wrapper is therefore compiled with dont_inherit plus exactly the bits the
# user source declares, so co_flags matches CPython's compile() of the source.
_FUTURE_BITS = {
    "division": 0x20000,
    "absolute_import": 0x40000,
    "with_statement": 0x80000,
    "print_function": 0x100000,
    "unicode_literals": 0x200000,
    "barry_as_FLUFL": 0x400000,
    "generator_stop": 0x800000,
    "annotations": 0x1000000,
}


def _future_flags(tree: ast.Module) -> int:
    bits = 0
    for node in tree.body:
        if isinstance(node, ast.ImportFrom) and node.module == "__future__":
            for alias in node.names:
                bits |= _FUTURE_BITS.get(alias.name, 0)
    return bits


def _enclosing_flags(stack: list[str]) -> int:
    # CO_NESTED is set when a FUNCTION scope sits anywhere above the body --
    # a method of a class nested in a function carries BOTH NESTED and METHOD
    # (measured against CPython 3.14's symtable, not guessed). CO_METHOD is
    # set only when the IMMEDIATE enclosing scope is a class.
    flags = 0
    if "<locals>" in stack:
        flags |= _CO_NESTED
    if stack and stack[-1] != "<locals>":
        flags |= _CO_METHOD
    return flags


def _remap_nested_qualnames(top: types.CodeType, wrapper_qual: str) -> types.CodeType:
    """Rebuild top's const tree with the wrapper's qualname prefix replaced
    by top's real qualname. The wrapper names every nested code object
    `_pyc_wrap.<locals>....`; tracebacks and reprs must show the real
    placement, so the prefix is rewritten bottom-up."""
    prefix = wrapper_qual

    def fix(c: types.CodeType) -> types.CodeType:
        new_consts = []
        for k in c.co_consts:
            if isinstance(k, types.CodeType) and k.co_qualname.startswith(prefix + "."):
                new_consts.append(fix(k.replace(
                    co_qualname=top.co_qualname + k.co_qualname[len(prefix):])))
            else:
                new_consts.append(k)
        if new_consts == list(c.co_consts):
            return c
        return c.replace(co_consts=tuple(new_consts))

    return fix(top)


def _compile_genfunc(node, frees: list[str], qualname: str, filename: str,
                     is_async: bool = False, enclosing_flags: int = 0,
                     future_flags: int = 0) -> tuple[bytes, list[str]]:
    shim = copy.deepcopy(node)
    # The shim is RENAMED before it goes into the wrapper, and its real name is
    # restored on the code object afterwards.
    #
    # Without this the wrapper changed the meaning of the function's own name.
    # `def powerset(U): ... for S in powerset(U): ...` at module level is a
    # GLOBAL self-reference, and symtable says so -- no free variables. Nest
    # that same def inside `_pyc_wrap` and `powerset` becomes a local of the
    # wrapper, so the compiled generator captures it as a freevar instead, and
    # the two disagreed:
    #
    #     'powerset' freevars ('powerset',) are not the same SET as symtable ()
    #
    # With the shim renamed, the self-reference resolves exactly as it did in
    # the original source: to the global when it was a global, and to the
    # wrapper PARAMETER when the name really is free, since a free name is
    # already passed in as one. Both cases come out matching symtable.
    shim.name = "_pyc_genfn"
    # Decorators, defaults and annotations are all evaluated by the ENCLOSING
    # scope at def time. Compiling them into the wrapper would evaluate them in
    # the wrong scope, at the wrong moment, or both. pyc supplies the defaults
    # through the function object and applies the decorators itself.
    shim.decorator_list = []
    shim.type_params = []
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

    mod = compile(wrapper, filename, "exec", flags=future_flags,
                  dont_inherit=True)
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
    # The wrapper compilation places the body inside `_pyc_wrap`, so its
    # CO_NESTED describes the wrapper, not the source, and no source
    # placement ever yields METHOD inside a wrapper. Rewrite the scope bits
    # with the real enclosing context. co_name would read "_pyc_genfn" and
    # co_qualname "_pyc_wrap.<locals>..."; both are corrected at build time
    # so tracebacks and repr name the real function, and the nested code
    # objects' wrapper-derived qualnames are remapped the same way.
    wrapper_qual = inner.co_qualname
    flags = (inner.co_flags & ~(_CO_NESTED | _CO_METHOD)) | enclosing_flags
    inner = inner.replace(co_name=node.name, co_qualname=qualname,
                          co_flags=flags)
    inner = _remap_nested_qualnames(inner, wrapper_qual)
    return (marshal.dumps(inner), list(inner.co_freevars))
