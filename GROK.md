# GROK.md

This project (pyc) was built interactively with Grok (xAI model grok-4.20-0309-reasoning) using the opencode CLI tool.

## Collaboration Summary
- **Scaffolding:** Grok created directory structure (src/frontend, ir, codegen, runtime, include/pyc, tests, cmake), CMake with LLVM 18 + Python C API integration, initial main, Compiler, PythonParser, IR, Codegen classes.
- **Frontend:** Parser using Python C API (`ast.parse`, recursive buildAST for Module, FunctionDef, Assign, Call, BinOp, Constant, Name, If, While, Compare, ClassDef, Try, Str). Handled PyObject traversal, fixed crashes/warnings with null checks and type handling.
- **Lowering:** AST to custom ModuleIR with instructions (const, add, assign, call, icmp, br, label, ret). Recursive lowerExpression with temp counters, value tracking, support for variables, control flow skeleton (if/else, while with labels/br).
- **Codegen:** LLVM IRBuilder for functions, constants, add, assign (alloca/store/load), calls (printf/PyObject_Print, general functions like add), control flow stubs (BasicBlock, ICmp, CondBr). TargetMachine for object emission with optimizations (O2 via Aggressive/Default). Verification and error handling.
- **Runtime:** Evolved from simple print to full `PyObject` (refcount, type, value), `PyInt_FromLong`, `PyNumber_Add`, `PyObject_Print`, `Py_DECREF`, `PyErr_Print`, stubs for exceptions/strings. Pure C for static linking.
- **CLI/Packaging:** Flags (--static, --opt, -o), CMake install target, automated linking with -static -s -Wl,--gc-sections.
- **Tests/Error Handling:** Comprehensive `tests/test_basic.py` + runner with timing benchmarks, output validation, error cases. Compiler/Parser handle parse errors, IR verification, link failures with messages.
- **Optimizations/Static:** LLVM passes via TargetMachine/PassBuilder (instcombine, GVN, CFG simplify). Static binaries (~700K, no dynamic libs via ldd).
- **Other:** Project rename (pyllvm -> pyc), variable/if/while/classes support, control flow skeleton, benchmarking, Apache 2.0 license, README, list/dict comprehension support.

Grok followed code conventions (no unnecessary comments, mimic existing style, use existing libs, security best practices). Used tools for search/edit/build/test/git. All changes verified with builds, runs, and tests.

The AI acted as an interactive engineering partner, providing plans, code, fixes, and reports after each step.

Project achieved MVC for Python subset to static native executables with LLVM.

### Later Major Work (with Grok)
- Visitor-based AST lowering refactor (Deep Polish 4)
- Full migration to `PyObject*` boxed runtime with basic refcounting (Deep Polish 5, Option A)
- Improved testing (`make check` / ctest), packaging (runtime headers + `libpycrt`), and CLI (`--emit-llvm`, `-S`, `--verbose`)

### Subsequent Major Work (with Claude / Anthropic)
The project was then extended substantially in collaboration with Claude Sonnet 4.6
(via the Claude Code CLI). Key additions:

- **Bug sweep**: fixed FunctionDef body parsing, Default node construction, vararg
  None checks, keyword arg name extraction, for-loop child indices, list literal
  construction (PyList_New was being called with raw string "3" as size → empty list)
- **Type system**: float/double (type 4), bool (type 5), `PyBool_New`, `PyStr_FromAny`
- **range() builtin**: `PyBuiltin_Range` with 1/2/3-arg normalization; boxed wrappers
  `PyList_SizeBoxed` / `PyList_GetItemObj` so for-loops stay in PyObject* world
- **String operations**: `+`, `*`, `len()`, `str()`, f-strings (`JoinedStr`/`FormattedValue`),
  `upper()`/`lower()`/`strip()`/`split()`/`join()`, `% formatting`
- **print() multi-arg**: space-joins via `PyStr_FromAny` + `PyString_Concat` in the compiler
- **and/or/not**: short-circuit BoolOp using result alloca + unique labels
- **Unary minus**: `PyNumber_Negate`
- **MVP gap fixes**: `elif` (fixed duplicate BasicBlock bug from fixed label names),
  subscript get/set, augmented assign (names + subscripts), `**` power, `in`/`not in`,
  ternary `x if c else y`, tuple unpack `a,b=1,2`, multi-value return, method calls
  (append/sort/pop/upper/lower/keys/values/items), `int()`/`float()`/`abs()` builtins
- **global statement**: two-pass pre-scan + LLVM `GlobalVariable` substitution in valueMap
- **Chained comparison**: parser stores all ops; lowerCompare evaluates operands once
- **List comprehensions**: rewrote lowerListComp to use real for-loop machinery
- **for i,v in enumerate(...)**: tuple-target for-loop via node->args
- **min/max/list/enumerate/zip**: new builtins
- **multi-target assign** `a=b=5`, **aug-assign on subscript** `a[i]+=1`
- **str % formatting**: `PyString_Format` hooked into `PyNumber_Remainder`
- **Symbol conflict fix**: renamed `PyObject_GetItem`/`SetItem`/`Contains`/`PyNumber_Power`
  to `Pyc_GetItem`/`Pyc_SetItem`/`Pyc_Contains`/`Pyc_Pow` to prevent them overriding
  CPython's own symbols during `Py_Initialize` (was causing SIGSEGV in MRO computation)

Test suite grew from ~5 cases to **137 passing tests** (all vs CPython output).

See README.md and LICENSE for details.
