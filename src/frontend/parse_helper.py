#!/usr/bin/env python3
"""Helper script for pyc compiler - parses Python source and outputs JSON AST."""
import ast
import sys
import json

def node_to_dict(node):
    """Convert an AST node to a dictionary."""
    if node is None:
        return None
    result = {
        "type": type(node).__name__,
        "value": None,
        "op": None,
        "id": None,
        "lineno": getattr(node, "lineno", 0),
        "is_float": False,
        "is_str": False,
        "is_bool": False,
        "args": [],
        "children": [],
    }

    # Extract simple attributes
    if isinstance(node, ast.Constant):
        result["value"] = repr(node.value) if node.value is not None else ""
        if isinstance(node.value, bool):
            result["is_bool"] = True
            result["value"] = "True" if node.value else "False"
        elif isinstance(node.value, float):
            result["is_float"] = True
        elif isinstance(node.value, str):
            result["is_str"] = True
    elif isinstance(node, ast.Name):
        result["id"] = node.id
    elif isinstance(node, ast.Attribute):
        result["id"] = node.attr
    elif isinstance(node, ast.FunctionDef) or isinstance(node, ast.AsyncFunctionDef):
        result["id"] = node.name
    elif isinstance(node, ast.ClassDef):
        result["id"] = node.name
    elif isinstance(node, ast.Assign):
        result["id"] = ""
        result["args"] = []
    elif isinstance(node, ast.For):
        result["id"] = ""
    elif isinstance(node, ast.BinOp):
        result["op"] = type(node.op).__name__
    elif isinstance(node, ast.Compare):
        result["op"] = "Compare"
    elif isinstance(node, ast.BoolOp):
        result["op"] = type(node.op).__name__
    elif isinstance(node, ast.UnaryOp):
        result["op"] = type(node.op).__name__
    elif isinstance(node, ast.If):
        pass
    elif isinstance(node, ast.While):
        pass
    elif isinstance(node, ast.Return):
        pass
    elif isinstance(node, ast.Expr):
        pass
    elif isinstance(node, ast.Import):
        result["args"] = [n.name for n in node.names]
    elif isinstance(node, ast.ImportFrom):
        result["args"] = [n.name for n in node.names]
        result["id"] = node.module or ""
    elif isinstance(node, ast.IfExp):
        result["op"] = "IfExp"
    elif isinstance(node, ast.Subscript):
        result["op"] = "Subscript"
    elif isinstance(node, ast.ListComp):
        pass
    elif isinstance(node, ast.DictComp):
        pass
    elif isinstance(node, ast.Lambda):
        pass
    elif isinstance(node, ast.AugAssign):
        result["op"] = type(node.op).__name__
    elif isinstance(node, ast.Global):
        result["args"] = node.names
    elif isinstance(node, ast.Nonlocal):
        result["args"] = node.names
    elif isinstance(node, ast.Call):
        pass

    # Recursively process children
    for field, value in ast.iter_fields(node):
        if isinstance(value, list):
            for item in value:
                if isinstance(item, ast.AST):
                    result["children"].append(node_to_dict(item))
        elif isinstance(value, ast.AST):
            result["children"].append(node_to_dict(value))

    return result

def main():
    if len(sys.argv) < 2:
        print("Usage: parse_helper.py <source.py>", file=sys.stderr)
        sys.exit(1)

    source_file = sys.argv[1]
    import os
    source_file = os.path.abspath(source_file)
    with open(source_file, "r") as f:
        source = f.read()

    try:
        tree = ast.parse(source, filename=source_file)
        result = node_to_dict(tree)
        print(json.dumps(result))
    except SyntaxError as e:
        print(f"ERROR:{e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"ERROR:{e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
