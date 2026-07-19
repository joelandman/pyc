# Complex Number Support Plan for pyc

**Goal:** Bring pyc to parity with Python's complex number support.

## Current State

### What Works
| Feature | Status |
|---------|--------|
| Integer arithmetic (+, -, *, /, //, %, **) | Supported (boxed + native int64) |
| Float arithmetic (+, -, *, /, //, %, **) | Supported (boxed + native double) |
| `pow()` builtin | Supported but NaN for complex results |
| `**` operator | Supported but NaN for complex results |
| NaN/inf display | Printed as "nan"/"inf" |

### What's Missing
| Feature | Status |
|---------|--------|
| Complex literal syntax (`3+4j`, `1j`) | Not implemented |
| `complex()` builtin | Not implemented |
| `TYPE_COMPLEX` PyObject type | Not implemented |
| Complex arithmetic (+, -, *, /) | Not implemented |
| `cmath` module | Not implemented |
| Complex-to-complex pow | Produces NaN |
| `abs()` for complex | Not implemented |

---

## Implementation Phases

### Phase 1: Core Type + Literals (Foundation)

**Goal:** Complex numbers as first-class values that can be created and printed.

#### 1.1 Runtime: `TYPE_COMPLEX`
- Add `TYPE_COMPLEX = 13` to `PyTypeKind` enum in `runtime/object.h`
- Add `complex_real` (double) and `complex_imag` (double) fields to PyObject
- Add `PyComplex_New(real, imag)` factory function in Runtime.cpp
- Add `PyComplex_Check(obj)` helper

#### 1.2 Lexer: Complex Literal Tokens
- Extend number parsing in `lexer.cpp` to detect `j`/`J` suffix
- Emit `COMPLEX_LITERAL` token with real=0, imag=value
- Handle `3+4j` syntax: parse as `BinOp(Constant(3), Add, ComplexLiteral(0, 4))`
  - Actually, Python parses `3+4j` as a single complex literal token
  - Need to detect `digit(s)j` or `digit(s)+digit(s)j` patterns

#### 1.3 Parser: ComplexLiteral AST Node
- Add `ComplexLiteral` AST node with `real` and `imag` string fields
- Parse complex literals in `PythonParser.cpp`
- Parse `3+4j` in the custom parser (`parser.cpp`)

#### 1.4 Compiler: Lower Complex Literals
- In `lowerExpr` (Constant case), handle complex literals
- Emit `call pyc_make_complex(real_str, imag_str)` to create type-13 objects
- Annotate result type as `"boxed"` (complex values always boxed initially)

#### 1.5 Runtime: Printing
- Update `PyObject_PrintBase` to handle type 13
- Format: `"(real+imagj)"` or `"(real-imagj)"` for negative imag
- Update `PyStr_FromAny` for `str()`/f-strings
- Update `PyBuiltin_Repr` for `repr()`

#### 1.6 Codegen: Complex Value Handling
- Add `getAsPyObject` path for complex (no unboxing)
- Ensure complex values flow through boxed path only

**Tests:**
```python
# Literals
print(1j)           # (0+1j)
print(3+4j)         # (3+4j)
print(2.5+1.5j)     # (2.5+1.5j)
print(1+0j)         # (1+0j)

# basic operations
x = 1j
print(x * x)        # (-1+0j)
```

---

### Phase 2: Arithmetic Operators

**Goal:** Complex numbers support +, -, *, / operators.

#### 2.1 Runtime: Complex Arithmetic Functions
Add to Runtime.cpp:
- `PyComplex_Add(a, b)` — returns new complex
- `PyComplex_Sub(a, b)` — returns new complex
- `PyComplex_Mul(a, b)` — returns new complex
- `PyComplex_Div(a, b)` — returns new complex (with zero-division check)

Formulas:
```
(a+bj) + (c+dj) = (a+c) + (b+d)j
(a+bj) - (c+dj) = (a-c) + (b-d)j
(a+bj) * (c+dj) = (ac-bd) + (ad+bc)j
(a+bj) / (c+dj) = ((ac+bd)/(c²+d²)) + ((bc-ad)/(c²+d²))j
```

#### 2.2 Compiler: Complex Arithmetic in lowerBinOp
- Detect when both operands are complex (type 13)
- Emit appropriate arithmetic call (`PyComplex_Add`, etc.)
- Result type is `"boxed"` (complex)

#### 2.3 Codegen: Complex Arithmetic
- Emit calls to `PyComplex_Add`, `PyComplex_Sub`, etc.
- No native path initially (all boxed)

**Tests:**
```python
a = 1+2j
b = 3+4j
print(a + b)        # (4+6j)
print(a - b)        # (-2-2j)
print(a * b)        # (-5+10j)
print(a / b)        # (0.44+0.08j)
```

---

### Phase 3: pow() and abs()

**Goal:** Handle `**` and `abs()` for complex numbers.

#### 3.1 Runtime: pow for Complex
- Extend `PyBuiltin_Pow` to detect complex operands
- Use `std::powl` / `std::pow` with complex types from `<complex>` header
- Or implement manually: `z1 ** z2 = exp(z2 * log(z1))`

#### 3.2 Runtime: abs() for Complex
- Extend `builtin_abs` to detect complex
- Return `sqrt(real² + imag²)` as float

#### 3.3 Compiler: pow/abs for Complex
- In `lowerBinOp` (Pow case), detect complex operands
- In `lowerCall` (abs case), detect complex operand

**Tests:**
```python
print((-1) ** 0.5)    # (6.12e-17+1j) ≈ 1j (with floating point error)
print(abs(3+4j))      # 5.0
print(abs(1j))        # 1.0
```

---

### Phase 4: complex() Builtin

**Goal:** `complex()` constructor for type conversion.

#### 4.1 Runtime: `PyBuiltin_Complex`
- `complex()` — returns 0+0j
- `complex(3)` — returns 3+0j
- `complex(3, 4)` — returns 3+4j
- `complex("3+4j")` — parse string (simplified: only numeric strings)
- `complex(3.5)` — returns 3.5+0j
- `complex(3+4j)` — returns same complex

#### 4.2 Compiler: Wire up in lowerCall
- Add special case for `complex` builtin name
- Emit appropriate `PyBuiltin_Complex` call

**Tests:**
```python
print(complex())        # (0+0j)
print(complex(3))       # (3+0j)
print(complex(3, 4))    # (3+4j)
print(complex(1j))      # (0+1j)
```

---

### Phase 5: cmath Module (Optional, Lower Priority)

**Goal:** Provide `cmath` module with complex-aware math functions.

#### 5.1 Option A: Python Implementation
- Create `cmath.py` with Python implementations
- Functions: `sqrt`, `log`, `exp`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`

#### 5.2 Option B: C Implementation
- Create `cmath.cpp` with C++ implementations using `<complex>` and `<cmath>`
- Register as a built-in module

**Priority:** Low — most complex number use cases are covered by Phases 1-4.

---

## Implementation Order

1. **Phase 1** (Core type + literals) — Foundation, ~2-3 hours
2. **Phase 2** (Arithmetic) — Core functionality, ~2 hours
3. **Phase 3** (pow/abs) — Important operators, ~1-2 hours
4. **Phase 4** (complex() builtin) — Convenience, ~1 hour
5. **Phase 5** (cmath) — Optional polish, ~2-3 hours

**Total estimated effort: ~8-11 hours**

## Risks and Guardrails

1. **Mixed arithmetic:** `1 + 2j` should work (int + complex → complex). Need type promotion rules.
2. **Unboxing interactions:** Complex values should never be unboxed to native types. Ensure `getAsPyObject` handles type 13 correctly.
3. **Homogeneous lists:** Complex values in lists should use boxed storage (no ilist/flist for complex).
4. **Division by zero:** `a / 0j` should raise ZeroDivisionError.
5. **NaN/inf in complex:** `complex(nan, 1)` should be valid. `complex(inf, inf)` should be valid.
6. **String formatting:** Complex formatting with f-strings should work correctly.

## Testing Strategy

Every phase must pass:
- `cd build && make check` (full test suite)
- `tests/runner.py` comparison against CPython
- New test file: `tests/complex.py` with comprehensive cases
- Verify output identity with CPython (within floating-point tolerance)

## Files to Modify

| File | Changes |
|------|---------|
| `runtime/object.h` | Add `TYPE_COMPLEX`, complex fields to PyObject |
| `src/runtime/Runtime.cpp` | Complex arithmetic, printing, pow, abs, complex() |
| `include/pyc/runtime.h` | Declare new functions |
| `frontend/lexer.cpp` | Complex literal token parsing |
| `frontend/lexer.h` | Add `COMPLEX_LITERAL` token |
| `frontend/parser.cpp` | Parse `3+4j` syntax |
| `frontend/ast.h` | Add `ComplexLiteral` node |
| `src/frontend/PythonParser.cpp` | Parse complex literals from Python AST |
| `src/Compiler.cpp` | Lower complex literals, arithmetic, pow, abs, complex() |
| `src/codegen/Codegen.cpp` | Handle type 13 in getAsPyObject, etc. |
| `ir/IR.h` | Possibly add complex result type |
| `ir/interpreter.cpp` | Handle complex in interpreter |
| `tests/complex.py` | New test file |
| `tests/runner.py` | Add complex.py to FILE_CASES |
