# pyc Debug Info Plan (`-g` flag)

Enable source-level debugging of compiled pyc binaries. When `-g` is passed,
emit DWARF debug info that maps LLVM IR instructions back to Python source
lines and variable names, so users can step through Python code in `gdb`,
`lldb`, or any DWARF-aware debugger.

## Goals

1. **Source line tracking**: every LLVM IR instruction emitted for a Python
   statement carries a debug location `(file, line)` pointing back to the
   original `.py` source. Debuggers show the Python source line, not the
   generated C++/LLVM IR.

2. **Variable name tracking**: Python-level variable names (`x`, `n`, `bodies`)
   appear in the debugger's variable list with their `PyObject*` value. The
   debugger can `print x` and see the boxed Python object (via a custom
   pretty-printer or by inspecting the struct fields).

3. **Function name mapping**: Python function names (`fib`, `advance`,
   `__module__`) map to the corresponding LLVM functions so stack traces show
   Python-level frames.

4. **No impact on non-debug builds**: `-g` is opt-in. Without it, no debug
   info is emitted and compilation is identical to today.

## Design

### Data Flow

```
Python source (.py)
    │  ast.parse (PythonParser.cpp)
    ▼  ASTNode (with lineno)
    │  LoweringVisitor (Compiler.cpp)
    ▼  IRInstruction (with lineno)        ← NEW: propagate lineno from AST to IR
    │  Codegen::generate (Codegen.cpp)
    ▼  LLVM IR (with llvm::DILocation)    ← NEW: attach debug locs + DIBuilder
    │  emitObject / link
    ▼  native executable (with DWARF)
```

### Layer 1: AST → IR line number propagation

**`include/pyc/IR.h`** — add `lineno` field to `IRInstruction` and `IRFunction`:

```cpp
struct IRInstruction {
    std::string op;
    std::vector<IRValue> operands;
    std::string result;
    std::string resultType;
    int lineno = 0;              // NEW: source line from ASTNode
};

struct IRFunction {
    // ... existing fields ...
    int defLineno = 0;           // NEW: line of `def` / function start
    std::string sourceFile;      // NEW: source file path for this function
};
```

**`src/ir/IR.cpp`** — `addInstruction` gets an optional `lineno` parameter
(default 0, no behavior change for existing callers):

```cpp
void ModuleIR::addInstruction(const std::string& funcName,
                              const std::string& op,
                              const std::vector<std::string>& operands,
                              const std::string& result,
                              const std::string& resultType,
                              int lineno);               // NEW
```

**`src/Compiler.cpp`** — track current source line during lowering:

- Add a `currentLineno` member to `LoweringVisitor`, updated at the start of
  `lower()` and `lowerExpr()` from `node->lineno`.
- A helper macro or inline function `emitInst(...)` wraps `ir.addInstruction`
  to automatically pass `currentLineno`.
- For `FunctionDef` lowering, set `fnr.defLineno = node->lineno` and
  `fnr.sourceFile = currentSourceFile`.
- The `lower()` method sets `currentLineno = node->lineno` at the top (if
  non-zero), so all IR instructions emitted while processing that node inherit
  the line. Child nodes with their own `lineno` update it further.
- **Scope**: ~100 call sites of `ir.addInstruction` need to pass lineno. The
  wrapper approach minimizes churn. Alternatively, `currentLineno` is read
  inside `addInstruction` itself (the `LoweringVisitor` holds a reference to
  the IR, so it can inject the current line automatically without changing
  any call site).

**Preferred approach** (minimal call-site changes): `ModuleIR::addInstruction`
reads the current line from a thread-local or visitor-level `currentLineno`
that the visitor sets before each `lower()` / `lowerExpr()` call. This way
**no call site changes are needed** — the line is injected automatically.

### Layer 2: IR → LLVM debug info (Codegen)

**`src/codegen/Codegen.cpp`** — when `-g` is enabled, use `llvm::DIBuilder`
to emit DWARF debug metadata.

#### Setup (once per module)

```cpp
// In Codegen::generate, when debugInfo flag is set:
llvm::DIBuilder dib(*module);

// Create the compile unit
llvm::DICompileUnit* cu = dib.createCompileUnit(
    llvm::dwarf::DW_LANG_C_plus_plus,   // or a custom Python language code
    dib.createFile(sourceFilename, sourceDir),
    "pyc",                               // producer
    false,                               // isOptimized
    "",                                  // flags
    0);                                  // runtime version

// Store the DIFile for per-function use
// Store the DIBuilder for the duration of generate()
```

#### Per-function

```cpp
// For each IRFunction f:
llvm::DISubprogram* subprog = dib.createFunction(
    cu,                                  // scope (compile unit)
    f.name,                              // function name (e.g. "fib")
    llvmFunctionName,                    // linkage name (e.g. "fib")
    diFile,                              // file
    f.defLineno,                         // line where function is defined
    subroutineType,                      // DISubroutineType
    0,                                   // scope line
    llvm::DINode::FlagPrototyped,
    llvm::DISubprogram::SPFlagDefinition);
func->setSubprogram(subprog);

// Create lexical block for the function body
llvm::DILexicalBlock* block = dib.createLexicalBlock(
    subprog, diFile, f.defLineno, 0);
```

#### Per-IR-instruction

For each IR instruction with a non-zero `lineno`, set the debug location on
the LLVM instruction after creating it:

```cpp
if (inst.lineno > 0 && debugEnabled) {
    llvm::DebugLoc loc = llvm::DILocation::get(context, inst.lineno, 0, block);
    llvmInstr->setDebugLoc(loc);
}
```

This is applied in the main instruction dispatch loop in `Codegen::generate`
(line ~786 onwards). The cleanest approach: after each `builder.Create*` call
that produces an instruction, check the current IR instruction's `lineno` and
set the debug loc. A helper lambda `setDebugLoc(inst)` wraps this.

#### Variable tracking

For each Python variable that has an LLVM alloca (slot), create a
`llvm::DbgDeclareInst` so the debugger knows the variable's location:

```cpp
// For each alloca created for a user variable (e.g. %x.slot):
llvm::DILocalVariable* diVar = dib.createAutoVariable(
    block,                // scope
    varName,              // "x", "n", "bodies"
    diFile,
    defLineno,            // line where the variable is first seen
    diType,               // PyObject* type (or i64/double for native)
    false);               // alwaysPreserve
dib.insertDeclare(alloca, diVar, dib.createExpression(),
                  llvm::DebugLoc::get(defLineno, 0, block),
                  &func->getEntryBlock());
```

The DI type for boxed variables is a pointer to the `PyObject` struct. For
native i64/double variables, it's the corresponding basic type. The debugger
will show the raw value; a pretty-printer (see below) can format `PyObject*`
as a Python repr.

#### Finalize

After all functions are generated, before returning the module:

```cpp
if (debugEnabled) {
    dib.finalize();
}
```

### Layer 3: Compile/link flags

**`src/main.cpp`** — add `-g` flag:

```cpp
else if (arg == "-g") debugInfo = true;
```

**`src/Compiler.cpp`** — pass `debugInfo` to `Codegen::generate`:

```cpp
auto module = codegen.generate(ir, context, moduleName, debugInfo);
```

**Link command** — add `-g` to the clang++ link line so the final binary
contains the DWARF sections:

```cpp
if (debugInfo) linkCmd += " -g ";
```

When `-g` is combined with `-O0` (the expected debug mode), LTO is off and
LLVM passes are off, so the debug info is maximally accurate (no
optimization moves instructions around). With `-g -O2`, some debug info may
be less precise (inlined functions, optimized-out variables), but line
mapping still works.

### Layer 4: Pretty-printers (optional, future)

A GDB pretty-printer for `PyObject*` would format the value as Python `repr()`
output. This can be a Python script loaded into `.gdbinit`:

```python
class PyObjectPrinter:
    def __init__(self, val):
        self.val = val
    def to_string(self):
        type_tag = int(self.val['type'])
        if type_tag == 0:  # int
            return str(int(self.val['value']))
        elif type_tag == 4:  # float
            return str(float(self.val['dvalue']))
        elif type_tag == 3:  # str
            return self.val['str'].string()
        # ... etc
```

This is not part of the core implementation but would be documented as a
usage tip.

## Implementation Plan

### Phase 1: IR line number propagation (Compiler.cpp + IR.h + IR.cpp)

- [ ] Add `lineno` field to `IRInstruction` and `IRFunction`
- [ ] Add `currentLineno` member to `LoweringVisitor`
- [ ] Set `currentLineno` from `node->lineno` at the start of `lower()` and
      `lowerExpr()` (if non-zero)
- [ ] Have `addInstruction` read `currentLineno` from the visitor (via a
      visitor-level method or by passing it through)
- [ ] Set `fnr.defLineno` and `fnr.sourceFile` during `FunctionDef` lowering
- [ ] Verify: dump IR with line numbers for a test file

**Scope**: ~3 files, ~50 lines of new code. No call-site changes if the
visitor injects `currentLineno` automatically.

### Phase 2: DIBuilder infrastructure (Codegen.cpp + Codegen.h)

- [ ] Add `debugInfo` parameter to `Codegen::generate()`
- [ ] Create `DIBuilder`, `DICompileUnit`, `DIFile` at module start
- [ ] Create `DISubprogram` per function with the function's `defLineno`
- [ ] Create `DILexicalBlock` per function
- [ ] After each IR instruction is lowered to LLVM IR, set `DebugLoc` from
      `inst.lineno`
- [ ] Call `dib.finalize()` before returning the module
- [ ] Verify: `llvm-dwarfdump` shows line tables on the output object

**Scope**: ~1 file (Codegen.cpp), ~80 lines of new code. The instruction
loop already exists; this adds a `setDebugLoc` call per instruction.

### Phase 3: Variable tracking (Codegen.cpp)

- [ ] For each alloca created for a user variable (params, locals, globals),
      emit `llvm::DbgDeclareInst` with the variable's Python name
- [ ] Map the alloca name back to the Python variable name (strip `.slot`
      suffix, map `__pyc_global_X` → `X`)
- [ ] Create DI types: `PyObject*` for boxed, `i64` for int natives,
      `double` for float natives
- [ ] Verify: `gdb` shows local variables in frame info

**Scope**: ~1 file, ~60 lines. The allocas are created in a known location
(function entry block setup); insert declare calls there.

### Phase 4: CLI + link flags (main.cpp + Compiler.cpp)

- [ ] Add `-g` flag to `main.cpp`
- [ ] Add `debugInfo` parameter to `Compiler::compile()`
- [ ] Pass `debugInfo` to `Codegen::generate()`
- [ ] Add `-g` to the clang++ link command
- [ ] Update usage text and README
- [ ] Verify: `gdb` can `break fib`, `step`, `next`, `print n` on a compiled
      binary

**Scope**: ~2 files, ~15 lines.

### Phase 5: Testing and documentation

- [ ] Test: compile `tests/fibn.py` with `-g -O0`, run under `gdb`
  - `break fib` → stops at the Python `def fib` line
  - `step` → steps through Python source lines
  - `print n` → shows the `PyObject*` (or i64 in specialized variant)
  - `backtrace` → shows `fib`, `pyc_user_main`, `main`
- [ ] Test: `-g -O2` still works (debug info may be less precise but present)
- [ ] Test: no `-g` → binary is identical to today (no debug sections)
- [ ] Test: 300/300 runner tests pass with and without `-g`
- [ ] Document `-g` in README.md and IMPLEMENTATION.md

## Constraints and Considerations

### Interaction with optimization

- **`-g -O0`**: ideal debug mode. No LTO, no LLVM passes. Debug info is
  maximally accurate. This is the recommended debug configuration.
- **`-g -O2`**: LTO + O2 may inline functions, move instructions, and
  eliminate variables. Line tables still work but some variables may be
  "optimized out". This is acceptable — it matches gcc/clang behavior.
- **`-g -O3`**: similar to `-O2` but more aggressive. Same tradeoffs.

### Interaction with specialization (A6)

Specialized variants (`__specialized_fib_i`) are synthetic functions with no
direct Python source line. They should either:
- Inherit the debug info from the original function (same `DIFile` and
  `defLineno`), so stepping into a specialized call still shows the Python
  source. The variant name is internal and the debugger maps it to the
  original function.
- Or be marked as artificial (`DINode::FlagArtificial`) so debuggers skip
  them in stack traces.

The first option is better for the user experience.

### Source file paths

The `DIFile` should use the absolute path of the input `.py` file so
debuggers can find the source. For imported modules, each function's
`sourceFile` field carries its own file path.

### Runtime functions

Runtime functions (`PyInt_FromLong`, `PyNumber_Add`, etc.) are in
`Runtime.cpp` which is compiled separately. When LTO is on, these get inlined
and the debug info from `Runtime.cpp` (if compiled with `-g`) would appear.
For `-g -O0` (no LTO), runtime calls are external and don't have debug info —
the debugger will step over them.

Optionally, the runtime bitcode (`runtime.bc`) could be compiled with `-g`
so runtime functions also have debug info. This is a nice-to-have, not
required for Phase 1.

### Limitations

- **No Python-level expression evaluation**: the debugger shows C/LLVM-level
  values (`PyObject*` structs, i64/double). A pretty-printer is needed for
  readable Python output. Full Python expression evaluation in the debugger
  would require embedding a Python interpreter — out of scope.
- **No breakpoint by Python expression**: breakpoints are by function name or
  source line (`break fib`, `break fibn.py:7`). Conditional breakpoints with
  Python expressions are not supported.
- **Specialized variants**: as noted above, synthetic functions may appear in
  stack traces unless marked artificial.

## File Impact Summary

| File | Change | Lines (est.) |
|------|--------|-------------|
| `include/pyc/IR.h` | Add `lineno` to IRInstruction, `defLineno`/`sourceFile` to IRFunction | ~5 |
| `include/pyc/Codegen.h` | Add `debugInfo` param to `generate()` | ~2 |
| `include/pyc/Compiler.h` | Add `debugInfo` param to `compile()` | ~1 |
| `src/ir/IR.cpp` | Pass `lineno` through `addInstruction` | ~5 |
| `src/Compiler.cpp` | `currentLineno` tracking, inject into IR | ~30 |
| `src/codegen/Codegen.cpp` | DIBuilder setup, per-instruction DebugLoc, variable declares | ~150 |
| `src/main.cpp` | `-g` flag parsing | ~5 |
| **Total** | | **~200** |

## Usage

```bash
# Compile with debug info
pyc hello.py -o hello -g -O0

# Debug with gdb
gdb ./hello
(gdb) break fib
(gdb) run 10
(gdb) next          # steps through Python source lines
(gdb) print n       # shows the PyObject* for n
(gdb) backtrace     # shows Python function names in frames

# Debug with lldb
lldb ./hello
(lldb) b fib
(lldb) run 10
(lldb) n
(lldb) frame variable n
```