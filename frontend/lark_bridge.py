"""
lark_bridge.py - Python script to use Lark for parsing Python source
and outputting an intermediate representation for the C++ compiler.

Run: python3 lark_bridge.py <source.py>
Output: AST in JSON format that the C++ compiler can consume
"""

import json
import sys
import os
from lark import Lark, Tree, Token

# Load grammar
GRAMMAR = r"""
start: module

?module: (statement)*

?statement: stmt_func_def
          | stmt_class_def
          | stmt_if
          | stmt_for
          | stmt_while
          | stmt_try_stmt
          | stmt_with
          | stmt_match
          | stmt_import
          | stmt_assign
          | stmt_augassign
          | stmt_return
          | stmt_raise
          | stmt_break
          | stmt_continue
          | stmt_pass
          | stmt_delete
          | stmt_global
          | stmt_nonlocal
          | stmt_assert
          | stmt_expr

// ===== FUNCTION DEFINITIONS =====
stmt_func_def: "def" NAME "(" [parameters] ")" ["->" test] ":" stmt_list

?parameters: parameter ("," parameter)* [","]
?parameter: NAME [":" test] ["=" test]
          | "*" NAME ["," NAME]
          | "**" NAME
          | test
          | args_star
?args_star: NAME ["," NAME]

// ===== CLASS DEFINITIONS =====
stmt_class_def: "class" NAME ["(" [arglist] ")"] ":" NEWLINE INDENT statements DEDENT
?arglist: argument ("," argument)*
?argument: test | NAME "=" test
?statements: (statement | simple_stmt)*

// ===== IF STATEMENT =====
stmt_if: "if" test ":" NEWLINE INDENT statements DEDENT 
       ("elif" test ":" NEWLINE INDENT statements DEDENT)*
       ["else" ":" NEWLINE INDENT statements DEDENT]

// ===== FOR STATEMENT =====
stmt_for: "for" exprlist "in" testlist ":" NEWLINE INDENT statements DEDENT
        ["else" ":" NEWLINE INDENT statements DEDENT]

// ===== WHILE STATEMENT =====
stmt_while: "while" test ":" NEWLINE INDENT statements DEDENT
          ["else" ":" NEWLINE INDENT statements DEDENT]

// ===== TRY/EXCEPT =====
stmt_try_stmt: "try" ":" NEWLINE INDENT statements DEDENT except_clause*
             ["finally" ":" NEWLINE INDENT statements DEDENT]
?except_clause: "except" [test ["as" NAME]] ":" NEWLINE INDENT statements DEDENT

// ===== WITH =====
stmt_with: "with" with_item ("," with_item)* ":" NEWLINE INDENT statements DEDENT
?with_item: test "as" NAME | test

// ===== ASSIGNMENT =====
stmt_assign: exprlist "=" testlist
stmt_augassign: expr AUGOP test

// ===== IMPORT =====
dotted_name: NAME ("." NAME)*
stmt_import: "import" dotted_name ("," dotted_name)*

// ===== CONTROL FLOW =====
stmt_return: "return" [testlist]
stmt_raise: "raise" [test ["from" test]]
stmt_break: "break"
stmt_continue: "continue"
stmt_pass: "pass"
stmt_delete: "del" exprlist
stmt_global: "global" NAME ("," NAME)*
stmt_nonlocal: "nonlocal" NAME ("," NAME)*
stmt_assert: "assert" test ["," test]

// ===== EXPRESSIONS =====
?test: or_expr
     | lambda_expr

?or_expr: and_expr ("or" and_expr)*
?and_expr: not_expr ("and" not_expr)*
?not_expr: "not" not_expr
         | comparison

?comparison: bitwise_or (comp_op bitwise_or)*
?comp_op: EQ | NE | LT | GT | LE | GE
        | "in" | "not" "in" | "is" ["not"]

?bitwise_or: xor_expr ("|" xor_expr)*
?xor_expr: and_expr ("^" and_expr)*
?and_expr2: shift_expr ("&" shift_expr)*
?shift_expr: add_expr (("<<" | ">>") add_expr)*
?add_expr: mul_expr (("=" | "/") mul_expr)*
?mul_expr: power (("*" | "/" | "//" | "%") power)*
?power: primary ("**" unary)*
?unary: ("+" | "-" | "~") unary | primary

?primary: atom ["[" [subscript] "]"] ["." NAME]
?atom: NAME | NUMBER | STRING
     | "(" [testlist] ")"
     | "[" [list_comp] "]"
     | "{" [dict_or_set] "}"

?list_comp: test "for" exprlist "in" testlist (comp_clause)*
?comp_clause: "if" test | comp_for
?comp_for: "for" exprlist "in" testlist (comp_clause)*

?dict_or_set: (dict_item | test) ("," (dict_item | test))*
?dict_item: test ":" test

?exprlist: expr ("," expr)*
?testlist: test ("," test)*
?lambda_expr: "lambda" [NAME ("," NAME)*] ":" test

// ===== LITERALS =====
NAME: /[^\W\d]\w*/
NUMBER: INT | FLOAT | COMPLEX
INT: /[1-9][0-9]*|0[xX][\da-fA-F]+|0[oO][0-7]+|0[bB][01]+/
FLOAT: /[0-9]+\.[0-9]+/
COMPLEX: /[0-9]+[jJ]/

%import common.ESCAPED_STRING as STRING
%import common.ASSIGNOP as AUGOP
%import common.WS_INLINE
%ignore WS_INLINE
%ignore COMMENT
%ignore NEWLINE
"""

# Build parser
parser = Lark(GRAMMAR, parser='earley', ambiguity='resolve', propagate_positions=True)

def ast_node_to_json(node, position=None):
    """Convert AST node to JSON-serializable format"""
    if isinstance(node, Tree):
        result = {
            'type': node.data,
            'line': node.line,
            'column': node.column,
            'end_line': node.end_line,
            'end_column': node.end_column,
        }
        children = []
        for child in node.children:
            children.append(ast_node_to_json(child, (node.line, node.column)))
        if children:
            result['children'] = children
        return result
    
    elif isinstance(node, Token):
        result = {
            'type': node.type,
            'value': node.value,
            'line': node.line,
            'column': node.column,
        }
        return result
    
    else:
        return {'type': type(node).__name__, 'value': str(node)}

def visit_statement(stmt_tree):
    """Visit statement and convert to JSON"""
    result = {'type': stmt_tree.data, 'line': stmt_tree.line}
    
    if stmt_tree.data == 'stmt_func_def':
        name = None
        params = []
        body = []
        return_type = None
        
        for child in stmt_tree.children:
            if isinstance(child, Tree):
                if child.data == 'NAME':
                    name = child.value
                elif child.data == 'parameter':
                    params.append(visit_parameter(child))
                elif child.data == 'statements':
                    body.append(visit_statement(c) for c in child.children if isinstance(c, Tree))
            elif isinstance(child, Token) and child.type == 'NAME':
                if child.value == '->':
                    continue
                return_type = str(child)
        
        result.update({
            'name': name,
            'params': params,
            'return_type': return_type,
            'body': list(body)
        })
    
    elif stmt_tree.data == 'stmt_import':
        modules = []
        for child in stmt_tree.children:
            if isinstance(child, Tree) and child.data == 'dotted_name':
                mod_name = '.'.join(str(c.value) for c in child.children if isinstance(c, Token))
                modules.append(mod_name)
        result['modules'] = modules
    
    return result

def get_grammar():
    """Return the grammar"""
    return GRAMMAR

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Usage: python3 lark_bridge.py <source.py> [output.json]')
        sys.exit(1)
    
    source_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    # Read source
    with open(source_file) as f:
        source = f.read()
    
    try:
        tree = parser.parse(source)
        # Convert to JSON
        result = ast_node_to_json(tree)
        
        # Pretty print
        output = json.dumps(result, indent=2)
        
        if output_file:
            with open(output_file, 'w') as f:
                f.write(output)
            print(f"AST written to {output_file}")
        else:
            print(output)
        
        print(f"\nParsed successfully! {len(tree.children)} statements found.")
        
    except Exception as e:
        print(f"Parse error: {e}")
        sys.exit(1)
