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
        compile(tree, args.file, "exec")
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

    json.dump({
        "schema_version": SCHEMA_VERSION,
        "python_version": "%d.%d.%d" % sys.version_info[:3],
        "feature_version": list(fv) if fv else None,
        "file": args.file,
        "ast": encode_node(tree),
    }, sys.stdout, indent=args.indent)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
