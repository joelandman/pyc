"""Parse boundary (INTERFACES.md §2.1).

Run BY THE TARGET INTERPRETER, so the AST is the target's by construction:

    <sysroot>/bin/python3.X -m pyc_parse FILE [--feature-version 3.Y]

Emits the JSON envelope on stdout. pyc itself links no libpython.
"""

from __future__ import annotations

import argparse
import ast
import json
import sys

from . import SCHEMA_VERSION, encode_node
import base64
import marshal
import types

from .genexp import (collect as collect_genexps, collect_genfuncs,
                     GenexpError)

# Comprehension and generator bodies are their own code objects. pyc either
# lowers those itself or already marshals them through the genexp table.
# Recording them here would pair a native def with the wrong code object.
_SKIP_CO_NAMES = {
    "<module>", "<listcomp>", "<setcomp>", "<dictcomp>", "<genexpr>",
}
_CO_OPTIMIZED = 0x0001
_CO_NEWLOCALS = 0x0002
_CO_GENERATOR = 0x0020
_CO_COROUTINE = 0x0080
_CO_ASYNC_GENERATOR = 0x0200


def _localsplus(co):
    """Slot order of co_localsplusnames, from the public name tuples.

    co_varnames is the CO_FAST_LOCAL prefix. Cell names absent from that
    prefix are the pure cells. Free vars are the tail. That is the inverse
    of the code constructor in Objects/codeobject.c.
    """
    names = list(co.co_varnames)
    have = set(names)
    for name in co.co_cellvars:
        if name not in have:
            names.append(name)
            have.add(name)
    names.extend(co.co_freevars)
    return names


def _code_end_col(co):
    last = 0
    for pos in co.co_positions():
        ecol = pos[3]
        if ecol is not None:
            last = ecol
    return last


def collect_func_codes(code):
    """Every ordinary function code object compile() produced.

    The native body keeps running. This object is installed as the
    function's __code__ so co_code, consts, names, the line table, and the
    exception table are the ones CPython's compiler emitted.
    """
    out = []

    def walk(co):
        flags = co.co_flags
        suspended = flags & (_CO_GENERATOR | _CO_COROUTINE | _CO_ASYNC_GENERATOR)
        if (co.co_name not in _SKIP_CO_NAMES and not suspended
                and (flags & _CO_OPTIMIZED) and (flags & _CO_NEWLOCALS)):
            out.append({
                "qual": co.co_qualname,
                "line": co.co_firstlineno,
                "end_col": _code_end_col(co),
                "locals": _localsplus(co),
                "freevars": list(co.co_freevars),
                "code": base64.b64encode(marshal.dumps(co)).decode("ascii"),
            })
        for c in co.co_consts:
            if isinstance(c, types.CodeType):
                walk(c)

    walk(code)
    return out


def collect_annotate_consts(code):
    out = []
    def walk(co):
        for c in co.co_consts:
            if not isinstance(c, types.CodeType):
                continue
            if c.co_name == "__annotate__":
                out.append({
                    "line": co.co_firstlineno,
                    "col": -1,
                    "code": base64.b64encode(marshal.dumps(c)).decode("ascii"),
                })
            else:
                walk(c)
    walk(code)
    return out

# Real code nests deeper than CPython's default 1000 frames allows once each
# AST level costs several: sympy's resolvent_lookup.py reaches depth 568.
# ast.parse itself copes; a recursive encoder does not, without this.
sys.setrecursionlimit(60000)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="pyc_parse")
    ap.add_argument("file")
    ap.add_argument("--feature-version", default=None,
                    help="restrict accepted syntax to this X.Y (the -std level)")
    ap.add_argument("--indent", type=int, default=None)
    args = ap.parse_args(argv)

    fv = None
    if args.feature_version:
        try:
            major, minor = (int(x) for x in args.feature_version.split(".", 1))
        except ValueError:
            print(f"pyc_parse: bad --feature-version {args.feature_version!r}",
                  file=sys.stderr)
            return 2
        if (major, minor) > sys.version_info[:2]:
            # feature_version only restricts downward; a parser cannot read
            # syntax newer than itself (VERSION_TARGETING.md, fact 2).
            print(f"pyc_parse: cannot target {args.feature_version} with a "
                  f"{sys.version_info[0]}.{sys.version_info[1]} interpreter",
                  file=sys.stderr)
            return 2
        fv = (major, minor)

    try:
        src = open(args.file, "rb").read()
    except OSError as e:
        print(f"pyc_parse: {e}", file=sys.stderr)
        return 2

    try:
        tree = ast.parse(src, filename=args.file, **({"feature_version": fv} if fv else {}))
        # ast.parse is NOT the whole of Python's syntax. `return` outside a
        # function, `yield` outside a function, `await` outside async, a
        # duplicate parameter name: ast.parse accepts all of them and compile()
        # rejects them. Parsing alone would let pyc compile programs CPython
        # refuses to run. Compiling to bytecode and throwing it away is the
        # cheapest way to inherit those rules instead of reimplementing them
        # (I3: use the protocol, do not re-derive it at the callsite).
        #
        # feature_version deliberately does NOT apply here: it is a parser
        # option, and this call exists only for its checks.
        # compile(source), not compile(tree): this module has
        # `from __future__ import annotations`, which would leak into
        # compile(tree) and stringify every annotation (I1).
        compiled = compile(src, args.file, "exec", dont_inherit=True)
    except SyntaxError as e:
        # Structured so the driver can render a §1.1 Diagnostic rather than
        # reformatting a traceback.
        json.dump({"schema_version": SCHEMA_VERSION, "error": {
            # The exact class, not the base: CPython distinguishes
            # IndentationError and TabError from SyntaxError, and reporting
            # the base for all three loses what the user needs to see.
            "kind": type(e).__name__, "message": e.msg, "file": e.filename,
            "line": e.lineno, "col": e.offset,
        }}, sys.stdout)
        sys.stdout.write("\n")
        return 1

    # Generator expressions are compiled here, by the TARGET interpreter, and
    # carried as marshalled code objects (rebuild/GENERATORS.md). They travel
    # in a side table keyed by source position rather than as extra AST
    # fields: the typed AST is generated from CPython's own ASDL and stays
    # faithful to it, so synthetic fields have no place in it.
    try:
        genexps = collect_genexps(tree, src, args.file) \
                + collect_genfuncs(tree, src, args.file) \
                + collect_annotate_consts(compiled)
    except GenexpError as e:
        json.dump({"schema_version": SCHEMA_VERSION, "error": {
            "kind": "GeneratorExpressionError", "message": str(e),
            "file": args.file, "line": 0, "col": 0,
        }}, sys.stdout)
        sys.stdout.write("\n")
        return 1

    json.dump({
        "schema_version": SCHEMA_VERSION,
        "python_version": "%d.%d.%d" % sys.version_info[:3],
        "feature_version": list(fv) if fv else None,
        "file": args.file,
        "genexps": genexps,
        "func_codes": collect_func_codes(compiled),
        "ast": encode_node(tree),
    }, sys.stdout, indent=args.indent)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
