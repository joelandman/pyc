# REMEDIATION.md

## Outstanding Issues Requiring Immediate Remediation

This document tracks correctness issues that must be fixed before the compiler can produce reliable binaries.

---

### Issue 1: GC `mark_object` Corrupts Refcounts

**Severity:** Critical  
**Location:** `runtime/gc.cpp:49-58`

**Problem:** `mark_object()` increments `obj->refcount` during the mark phase. This is fundamentally wrong — the mark phase should only set a mark bit, not modify refcounts. Incrementing refcounts during marking means:
- Refcounts are inflated beyond their true value
- `del_ref()` will never reach zero for marked objects
- Objects are never freed even when unreachable

```cpp
void GarbageCollector::mark_object(PyObject* obj) {
    if (!obj || mark_set_.count(obj)) return;
    mark_set_.insert(obj);
    obj->refcount++;  // BUG: corrupts refcount
    if (obj->next) {
        mark_object(obj->next);
    }
}
```

**Fix:** Remove the `obj->refcount++` line. The mark phase should only add to `mark_set_`.

---

### Issue 2: GC `del_ref` Logic is Inverted

**Severity:** Critical  
**Location:** `runtime/gc.cpp:66-74`

**Problem:** `del_ref()` deletes an object when it is NOT in the mark set. But the correct logic is: an object should be deleted during sweep when it is NOT marked (i.e., unreachable). The current code checks `!mark_set_.count(obj)` which means it tries to delete objects that were never marked — but this happens at refcount drop time, not during sweep. The GC never actually calls `del_ref()` from the sweep path.

```cpp
void GarbageCollector::del_ref(PyObject* obj) {
    if (!obj) return;
    obj->refcount--;
    if (obj->refcount <= 0 && !mark_set_.count(obj)) {
        delete obj;  // BUG: wrong condition, wrong timing
        swept_count_++;
    }
}
```

**Fix:** The sweep phase should iterate `roots_`, check `mark_set_`, and delete unmarked objects. `del_ref()` should only decrement refcount and delete when it reaches zero (without any mark set check).

---

### Issue 3: Roots Never Pruned During Sweep

**Severity:** Critical  
**Location:** `runtime/gc.cpp:26-35, 76-86`

**Problem:** `new_object()` adds every new object to `roots_`. The `collect()`/`mark_all_roots()` methods never remove dead objects from `roots_`. This means:
- The root set grows monotonically
- Even freed objects remain in roots (since they're never removed)
- `object_count()` always returns total allocations ever made

```cpp
PyObject* GarbageCollector::new_object(PyTypeKind kind, size_t size) {
    auto* obj = new PyObject();
    // ...
    roots_.push_back(obj);  // Objects never removed from roots_
    return obj;
}
```

**Fix:** After the sweep phase, rebuild `roots_` to contain only marked objects. Or better: remove the auto-registration of roots from `new_object()` and let the interpreter explicitly manage roots.

---

### Issue 4: `create_str()` Does Not Store the String

**Severity:** Critical  
**Location:** `runtime/object.cpp:60-68`

**Problem:** `create_str()` sets `obj->data = 0` with a comment "string stored separately" but there is no separate storage mechanism. The string value is completely lost. This means:
- All string objects have empty/zero data
- `builtin_str()` returns objects with no content
- `py_object_to_string()` in builtins.cpp casts `obj->data` (which is 0) as `char*`, causing undefined behavior

```cpp
PyObject* PyObjectFactory::create_str(PyObject* /*type_obj*/, const std::string& value) {
    auto obj = new PyObject();
    obj->refcount = 1;
    obj->type_object = static_cast<uint32_t>(TYPE_STR);
    obj->data = 0;  // BUG: string value is discarded
    obj->next = nullptr;
    return obj;
}
```

**Fix:** Add a `std::string*` or `char*` member to `PyObject`, or use a separate string table. Store the string value in the object.

---

### Issue 5: `create_function()` Does Not Store the Callable

**Severity:** High  
**Location:** `runtime/object.cpp:70-78`

**Problem:** `create_function()` receives a `std::function` parameter but discards it. The function object cannot be called because the callable is not stored.

```cpp
PyObject* PyObjectFactory::create_function(PyObject* /*type_obj*/, std::string /*name*/,
                                            std::function<PyObject*(PyObject*, std::vector<PyObject*>)> /*func*/) {
    auto obj = new PyObject();
    // ... all parameters discarded
    return obj;
}
```

**Fix:** Store the `std::function` in the PyObject, e.g., add `std::function<PyObject*(PyObject*, std::vector<PyObject*>)> callable;` to the PyObject struct or use `obj->data` to store a pointer to the callable.

---

### Issue 6: IR Builder Has Duplicate Function Definitions

**Severity:** High  
**Location:** `ir/builder.cpp:64-98` and `ir/builder.cpp:100-123`

**Problem:** `build_function()` is defined twice in the same file. The compiler may use either one (likely the second due to C++ ODR rules). Similarly, `build_expr()` is defined twice at lines 233-270 and 272-274. This is a compilation risk and indicates copy-paste errors.

**Fix:** Remove the duplicate definitions. Keep only the correct implementation.

---

### Issue 7: IR Builder Control Flow Is Incomplete

**Severity:** High  
**Location:** `ir/builder.cpp:166-194`

**Problem:** Loop and conditional control flow is not properly generated:
- `build_for_stmt()` creates blocks but never emits back-edges, condition checks, or jumps
- `build_while_stmt()` creates blocks but never evaluates the test expression or emits branches
- `build_if_stmt()` creates true/false blocks but no unconditional jump after each branch, causing fallthrough

**Fix:** Implement proper control flow:
- For loops: iter creation → loop_label → next() → branch on truth → body → jump back to label
- While loops: loop_label → test → branch → body → jump back to label
- If statements: branch → true_body → jump_to_merge → false_body → merge

---

### Issue 8: Interpreter Frame Ownership Is Broken

**Severity:** High  
**Location:** `ir/interpreter.cpp:187-234`

**Problem:** `call_function_impl()` allocates `CallFrame` with `new` (line 202) and pushes it to `frame_stack_`. Later, `handle_return()` pops from `frame_stack_`. Then `delete frame` is called at line 231. If the return handler already popped the frame, the pointer is still valid (just not in the stack). But if the frame is popped during a nested call, the comparison `frame_stack_.back() != frame` at line 222 breaks. The ownership model is unclear.

```cpp
auto* frame = new CallFrame(fn);  // Raw pointer, unclear ownership
push_frame(frame);
// ...
if (!frame_stack_.empty() && frame_stack_.back() == frame) {
    pop_frame();
}
delete frame;  // Double-free risk if pop_frame already consumed it
```

**Fix:** Use `std::unique_ptr<CallFrame>` stored in the frame stack, or use a memory arena/pool for call frames.

---

### Issue 9: LLVM Codegen Stubs Return Wrong Values

**Severity:** High  
**Location:** `codegen/ir2ll.cpp:350-363`

**Problem:** Critical Python operations return `i64(0)` stubs:
- `MAKE_LIST` — list creation returns 0 instead of a list object
- `LIST_GET`/`LIST_SET` — list access returns 0
- `NEWOBJ` — object creation returns 0
- `INTRINSIC_PRINT` — print returns 0
- `INTRINSIC_RANGE` — range returns 0
- `INTRINSIC_TYPE`/`INTRINSIC_LEN` — type/len return 0
- `POW` uses `CreateFMul` (multiplication) instead of power

**Fix:** Replace stubs with actual LLVM calls to runtime functions, or implement proper LLVM IR generation for each operation.

---

### Issue 10: `main.cpp` Has a Call-to-Function Bug

**Severity:** High  
**Location:** `main.cpp:214`

**Problem:** `args.output_file = argv[i++]();` attempts to call `argv[i++]` as a function. `argv` is `char**`, so `argv[i++]` returns `char*`, which is not callable. This will cause a compilation error.

```cpp
} else if (arg == "-o") {
    args.output_file = argv[i++]();  // BUG: calling char* as function
```

**Fix:** Remove the `()`: `args.output_file = argv[i++];`

---

### Issue 11: `main.cpp` Uses Wrong Option Flag

**Severity:** Medium  
**Location:** `main.cpp:97, 215`

**Problem:** The pipeline function is called with `emit_llvm_ir` but the CLI option is `--emit-llvm-ir` (line 215), while the usage message says `--emit-llir` (line 31). The option name in help doesn't match the parsed flag.

**Fix:** Make the help text match the actual flag, or vice versa.

---

### Issue 12: Interpreter `resolve_value` Does Linear Scan

**Severity:** Medium  
**Location:** `ir/interpreter.cpp:111-145`

**Problem:** `resolve_value()` iterates through ALL blocks and ALL instructions in the function to find the instruction producing a value. For a function with N instructions, this is O(N^2) per value resolution.

```cpp
for (auto* blk : func.blocks) {
    for (auto& instr : blk->instrs) {
        if (instr->id == val_ref) { ... }
    }
}
```

**Fix:** Build an instruction ID → instruction pointer map in the CallFrame or Interpreter, or use the `name_to_slot` map consistently.

---

### Issue 13: No `gc.h` Header File

**Severity:** Medium  
**Location:** `runtime/` directory

**Problem:** `gc.cpp` includes `runtime/gc.h` (line 1) but no such file exists. The GC class is defined in `runtime/object.h`. This will cause a compilation error.

**Fix:** Either create `runtime/gc.h` with the GC class declaration, or change the include to `runtime/object.h`.

---

### Issue 14: `main.cpp` Includes `ir/ir.h` Twice

**Severity:** Low  
**Location:** `main.cpp:16-17`

**Problem:** `#include "ir/ir.h"` appears on both lines 16 and 17. Harmless due to include guards but indicates sloppy code.

**Fix:** Remove the duplicate include.

---

### Issue 15: `PyObjectFactory::finalize()` Only Frees Singletons

**Severity:** Medium  
**Location:** `runtime/object.cpp:89-94`

**Problem:** `finalize()` only deletes singleton objects. All other objects created via `create_int()`, `create_str()`, `create_list()`, etc. are never freed. For programs that create many objects, this means memory is only reclaimed at process exit.

**Fix:** Either implement proper refcounting with automatic deletion, or track all non-singleton allocations and clean them up in `finalize()`.

---

### Issue 16: `build_class()` Creates Empty IR Functions

**Severity:** Medium  
**Location:** `ir/builder.cpp:125-142`

**Problem:** `build_class()` creates an empty `IRFunction` and stores it in `builtins_`. It does not generate any meaningful IR for class definitions. This means classes are parsed but produce no executable code.

**Fix:** Generate IR that creates a type object with method references and instance constructor.

---

### Issue 17: `AST::Module::classify_funcs_and_classes()` Creates Shared Pointers from Raw Pointers

**Severity:** Medium  
**Location:** `frontend/ast.cpp:12-14`

**Problem:** `functions_.push_back(std::shared_ptr<FunctionDef>(fd))` creates a new `shared_ptr` from a raw pointer `fd` which is `stmt.get()`. But `stmt` is already a `shared_ptr<Stmt>` in the body vector. Creating a second `shared_ptr` from the same raw pointer means two independent `shared_ptr`s manage the same object — when one is destroyed, it deletes the object while the other still references it (use-after-free).

```cpp
if (auto* fd = dynamic_cast<FunctionDef*>(stmt.get())) {
    functions_.push_back(std::shared_ptr<FunctionDef>(fd));  // BUG: double ownership
```

**Fix:** Use `std::shared_from_this()` if the AST nodes inherit from `enable_shared_from_this`, or use `std::static_pointer_cast` instead of raw pointer conversion.

---

### Issue 18: `wrap_numeric` Helper Has Template Argument Deduction Issue

**Severity:** Low  
**Location:** `ir/interpreter.h:158-165`

**Problem:** `wrap_numeric(int64_t a, int64_t b, auto op)` takes `int64_t` parameters but the body uses `std::holds_alternative<int64_t>(a)` which requires `a` to be a `PyValue`. The function signature doesn't match the usage — this will not compile as written. The function should take `PyValue` parameters.

**Fix:** Change signature to `wrap_numeric(PyValue a, PyValue b, auto op)` or fix the implementation to match the signature.

---

## Summary of Critical Issues

| # | Issue | Severity | File |
|---|-------|----------|------|
| 1 | GC `mark_object` corrupts refcounts | Critical | gc.cpp:53 |
| 2 | GC `del_ref` logic inverted | Critical | gc.cpp:70 |
| 3 | Roots never pruned | Critical | gc.cpp:84 |
| 4 | `create_str()` discards string value | Critical | object.cpp:64 |
| 5 | `create_function()` discards callable | Critical | object.cpp:70-78 |
| 6 | Duplicate `build_function`/`build_expr` | High | builder.cpp:64, 100, 233, 272 |
| 7 | Incomplete loop/if control flow | High | builder.cpp:166-194 |
| 8 | Frame ownership broken | High | interpreter.cpp:202-231 |
| 9 | LLVM codegen stubs return 0 | High | ir2ll.cpp:350-363 |
| 10 | `argv[i++]()` call-to-function bug | High | main.cpp:214 |
| 13 | Missing `gc.h` header | Medium | runtime/ |
| 17 | Double shared_ptr ownership | Medium | ast.cpp:13 |
