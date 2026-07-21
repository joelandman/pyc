#pragma once

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PyObject PyObject;

PyObject* PyInt_FromLong(long v);
PyObject* PyList_New(size_t size);
PyObject* PyList_GetItem(PyObject* list, size_t index);
size_t    PyList_Size(PyObject* list);
void      PyList_SetItem(PyObject* list, size_t index, PyObject* item);
void      Py_DECREF(PyObject* obj);
void      Py_INCREF(PyObject* obj);
int       PyObject_Print(PyObject* obj, FILE* fp);
PyObject* PyNumber_Add(PyObject* a, PyObject* b);
PyObject* PyNumber_Multiply(PyObject* a, PyObject* b);
PyObject* PyNumber_Subtract(PyObject* a, PyObject* b);
PyObject* PyNumber_Divide(PyObject* a, PyObject* b);
 PyObject* PyNumber_Remainder(PyObject* a, PyObject* b);
 PyObject* PyUnicode_FromString(const char* s);
 PyObject* PyObject_GetAttr(PyObject* obj, const char* attr);
 PyObject* PyObject_Call(PyObject* obj, PyObject* args, PyObject* kwargs);
 PyObject* PyDict_New(void);
void      PyDict_SetItem(PyObject* dict, PyObject* key, PyObject* value);
PyObject* PyDict_GetItem(PyObject* dict, PyObject* key);
PyObject* PyDict_GetItemWithDefault(PyObject* dict, PyObject* key, PyObject* defaultVal);
PyObject* PyDict_DelItem(PyObject* dict, PyObject* key);
PyObject* PyList_Append(PyObject* list, PyObject* item);
PyObject* PyList_FromArray(PyObject** items, size_t size);
PyObject* PyList_Range(int start, int end);
PyObject* PyList_SizeBoxed(PyObject* list);
PyObject* PyList_GetItemObj(PyObject* list, PyObject* idx);
PyObject* PyList_NewBoxed(PyObject* n);
PyObject* PyList_NewIntBoxed(PyObject* n);
PyObject* PyList_NewFloatBoxed(PyObject* n);
long      PyList_GetItemInt64(PyObject* list, size_t index);
double    PyList_GetItemDouble(PyObject* list, size_t index);
void      PyList_SetItemInt64(PyObject* list, size_t index, long v);
void      PyList_SetItemDouble(PyObject* list, size_t index, double v);
void      PyList_SetItemInt64Auto(PyObject* list, size_t index, long v);
void      PyList_SetItemDoubleAuto(PyObject* list, size_t index, double v);
void      PyList_SetItemBoxed(PyObject* list, PyObject* idx, PyObject* item);
PyObject* PyList_Comprehension(int start, int end);
PyObject* PyFloat_FromDouble(double v);
PyObject* PyNumber_TrueDivide(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Range(PyObject* start, PyObject* stop, PyObject* step);
int       PyObject_CompareBool(PyObject* a, PyObject* b, int op);
PyObject* PyStr_FromAny(PyObject* obj);
const char* PyStr_AsUTF8(PyObject* obj);
PyObject* Pyc_OsPathExists(PyObject* path);
PyObject* Pyc_OsPathIsFile(PyObject* path);
PyObject* Pyc_OsPathIsDir(PyObject* path);
PyObject* Pyc_OsUnlink(PyObject* path);
PyObject* Pyc_SubprocessCall(PyObject* cmdList);
PyObject* Pyc_SubprocessCheckOutput(PyObject* cmdList);
PyObject* PyString_Concat(PyObject* a, PyObject* b);
PyObject* PyString_Repeat(PyObject* s, PyObject* n);
PyObject* PyBuiltin_Len(PyObject* obj);
void PyBuiltin_PrintNewline(void);
void pyc_print(PyObject* argList, PyObject* sep, PyObject* end);
PyObject* pyc_import_failed(PyObject* modName);
PyObject* PyBool_New(int v);
PyObject* PyBuiltin_Sum(PyObject* lst);
PyObject* PyBuiltin_Sorted(PyObject* lst, PyObject* key);
PyObject* PyBuiltin_SortedWithCmp(PyObject* lst, PyObject* cmp);
PyObject* PyBuiltin_Any(PyObject* lst);
PyObject* PyBuiltin_All(PyObject* lst);
PyObject* Pyc_IsInstance(PyObject* obj, PyObject* typecode);
PyObject* PyString_Find(PyObject* s, PyObject* sub);
PyObject* PyString_Find3(PyObject* s, PyObject* sub, PyObject* start);
PyObject* PyString_RFind(PyObject* s, PyObject* sub);
PyObject* PyString_RFind3(PyObject* s, PyObject* sub, PyObject* start);
PyObject* PyString_RFind4(PyObject* s, PyObject* sub, PyObject* start, PyObject* end);
PyObject* PyString_Count(PyObject* s, PyObject* sub);
PyObject* PyString_Replace(PyObject* s, PyObject* old_, PyObject* new_);
PyObject* Pyc_GetSlice(PyObject* obj, PyObject* start, PyObject* stop, PyObject* step);
void      Pyc_SetSlice(PyObject* obj, PyObject* start, PyObject* stop, PyObject* step, PyObject* value);
PyObject* PyBuiltin_Min2(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Max2(PyObject* a, PyObject* b);
PyObject* PyBuiltin_MinList(PyObject* lst);
PyObject* PyBuiltin_MaxList(PyObject* lst);
PyObject* PyBuiltin_List(PyObject* obj);
PyObject* PyBuiltin_Reversed(PyObject* obj);
PyObject* PyBuiltin_Enumerate(PyObject* iterable);
PyObject* PyBuiltin_Zip2(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Int(PyObject* obj);
PyObject* PyBuiltin_IntBase(PyObject* obj, PyObject* base);
PyObject* PyBuiltin_Ord(PyObject* obj);
PyObject* PyBuiltin_Chr(PyObject* obj);
PyObject* PyBuiltin_Float(PyObject* obj);
PyObject* PyBuiltin_Complex(PyObject* obj1, PyObject* obj2);
PyObject* PyBuiltin_Abs(PyObject* obj);
PyObject* PyBuiltin_Id(PyObject* obj);
PyObject* PyBuiltin_Divmod(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Repr(PyObject* obj);
PyObject* PyBuiltin_Round(PyObject* x, PyObject* n);
PyObject* PyBuiltin_Pow(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Bool(PyObject* obj);
PyObject* PyBuiltin_Type(PyObject* obj);
PyObject* PyBuiltin_Hex(PyObject* obj);
PyObject* PyBuiltin_Oct(PyObject* obj);
 PyObject* PyBuiltin_Bin(PyObject* obj);
 PyObject* PyBuiltin_Super(void);
 PyObject* PyBuiltin_SuperMethod(PyObject* args);
 PyObject* PyObject_GetAttrExtended(PyObject* obj, PyObject* attr);
 PyObject* PyString_Upper(PyObject* s);
PyObject* PyString_Lower(PyObject* s);
PyObject* PyString_Strip(PyObject* s);
PyObject* PyString_LStrip(PyObject* s);
PyObject* PyString_RStrip(PyObject* s);
PyObject* PyString_Split(PyObject* s, PyObject* sep);
PyObject* PyString_SplitWhitespace(PyObject* s);
PyObject* PyString_Join(PyObject* sep, PyObject* iterable);
PyObject* PyString_StartsWith(PyObject* s, PyObject* prefix);
PyObject* PyString_EndsWith(PyObject* s, PyObject* suffix);
PyObject* PyString_IsAlpha(PyObject* s);
PyObject* PyString_IsDigit(PyObject* s);
PyObject* PyString_IsAlnum(PyObject* s);
PyObject* PyString_IsLower(PyObject* s);
PyObject* PyString_IsUpper(PyObject* s);
PyObject* PyString_IsSpace(PyObject* s);
PyObject* PyString_Casefold(PyObject* s);
PyObject* PyString_Title(PyObject* s);
PyObject* PyString_ZFill(PyObject* s, PyObject* w);
PyObject* PyString_Center(PyObject* s, PyObject* w, PyObject* fill);
PyObject* PyString_LJust(PyObject* s, PyObject* w, PyObject* fill);
PyObject* PyString_RJust(PyObject* s, PyObject* w, PyObject* fill);
PyObject* PyString_ReplaceN(PyObject* s, PyObject* old_, PyObject* new_, PyObject* count);
// A8: String formatting with % operator
PyObject* PyString_Format(PyObject* fmt, PyObject* args);
PyObject* PyDict_Keys(PyObject* d);
PyObject* PyDict_Values(PyObject* d);
PyObject* PyDict_Items(PyObject* d);
PyObject* PyDict_Update(PyObject* dst, PyObject* src);
PyObject* PyDict_SetDefault(PyObject* d, PyObject* key, PyObject* defval);
PyObject* PyDict_Copy(PyObject* d);
PyObject* PyDict_Clear(PyObject* d);
PyObject* PyDict_Pop(PyObject* d, PyObject* key, PyObject* defval);
PyObject* PyDict_PopItem(PyObject* d);
PyObject* PyDict_FromKeys(PyObject* keys, PyObject* defval);
PyObject* PyList_Sort(PyObject* lst);
PyObject* PyList_Pop(PyObject* lst);
PyObject* PyList_Insert(PyObject* list, PyObject* idx, PyObject* item);
PyObject* PyList_Remove(PyObject* list, PyObject* item);
PyObject* PyList_Index(PyObject* list, PyObject* item);
PyObject* PyList_Count(PyObject* list, PyObject* item);
PyObject* PyList_Reverse(PyObject* list);
PyObject* PyList_Extend(PyObject* list, PyObject* other);
PyObject* PyList_Copy(PyObject* list);
PyObject* PyList_Clear(PyObject* list);
PyObject* PyList_PopAt(PyObject* list, PyObject* idx);
PyObject* PyObject_TruthBoxed(PyObject* obj);
PyObject* Pyc_GetItem(PyObject* obj, PyObject* key);
PyObject* Pyc_Subscript(PyObject* obj, PyObject* key);
PyObject* Pyc_SetItem(PyObject* obj, PyObject* key, PyObject* val);
PyObject* Pyc_Contains(PyObject* container, PyObject* item);
PyObject* Pyc_Pow(PyObject* a, PyObject* b);
void pyc_register_class(PyObject* name, PyObject* cls);
int64_t Pyc_PowInt64(int64_t base, int64_t exp);
PyObject* Pyc_PowInt64Obj(int64_t base, int64_t exp);
PyObject* PyObject_Not(PyObject* obj);
PyObject* PyNumber_Negate(PyObject* obj);
void PyErr_Print(void);

// Exception handling using setjmp/longjmp.
// `pyc_try_push` records a jump buffer + a single exception type filter on
// the thread-local try stack; returns 0 for "first entry", non-zero when
// a raise has longjmp'd back to this buffer. The matching `pyc_try_pop`
// removes the buffer. `pyc_raise` walks the try stack and longjmps to the
// innermost matching buffer (or stores the exception if no match).
void      pyc_try_push(void* jmpBuf, PyObject* filterType);
void      pyc_try_pop(void);
void      pyc_raise(PyObject* exc);
void      pyc_reraise(void);
PyObject* pyc_current_exception(void);
void      pyc_clear_exception(void);
// Structured exceptions (type 10): typeName + optional message object.
PyObject* pyc_make_exc(PyObject* typeName, PyObject* msg);
// Function objects (type 11): callable token + display name for repr.
PyObject* pyc_make_func(PyObject* token, PyObject* displayName);
// Exception class objects (type 12): callable, constructs exceptions via pyc_make_exc.
PyObject* pyc_make_exc_class(PyObject* excName);
// Complex numbers (type 13): real and imaginary parts as doubles.
PyObject* PyComplex_New(double real, double imag);
// Complex arithmetic: returns new complex object (owned ref).
PyObject* PyComplex_Add(PyObject* a, PyObject* b);
PyObject* PyComplex_Sub(PyObject* a, PyObject* b);
PyObject* PyComplex_Mul(PyObject* a, PyObject* b);
PyObject* PyComplex_Div(PyObject* a, PyObject* b);
// Complex pow and abs
PyObject* PyComplex_Pow(PyObject* base, PyObject* exp);
PyObject* PyComplex_Abs(PyObject* z);
// cmath module functions
PyObject* PyCmath_Sqrt(PyObject* z);
PyObject* PyCmath_Log(PyObject* z);
PyObject* PyCmath_Exp(PyObject* z);
PyObject* PyCmath_Sin(PyObject* z);
PyObject* PyCmath_Cos(PyObject* z);
PyObject* PyCmath_Tan(PyObject* z);
// Boxed-bool: does exc match an except clause naming typeName (incl. the
// builtin hierarchy, e.g. except ArithmeticError catches ZeroDivisionError)?
PyObject* pyc_exc_matches(PyObject* exc, PyObject* typeName);

// Build a synthetic `sys` module from C argc/argv. The module has at
// least `sys.argv` as a list of Python strings. The C entry point
// (compiled binary's main) is expected to call this before user code
// runs.
void pyc_setup_sys(int argc, char** argv);
void pyc_setup_callables(void);

// Look up a name on the global `sys` module (or NULL if `pyc_setup_sys`
// has not been called or the attribute is missing). Returns a new
// strong reference on success.
PyObject* pyc_get_sys_attr(const char* name);

// Return the global `sys` module object (a new strong reference, or
// NULL if pyc_setup_sys has not been called).
PyObject* pyc_get_sys_module(void);

// B7: sys.modules support
// Get the sys.modules dict (a new strong reference, or NULL if not initialised).
PyObject* pyc_get_sys_modules(void);

// Add a module to sys.modules (increments refcount of module_dict).
void pyc_register_module(const char* name, PyObject* module_dict);

// B7: Runtime import support
// Import a module by name (returns module dict or NULL)
PyObject* pyc_import_module(const char* module_name);
// Import a specific name from a module
PyObject* pyc_import_from_module(const char* module_name, const char* name);
// Clear the module registry (for testing/cleanup)
void pyc_clear_modules(void);
// B7: Run a module's entry point by name (called by import handling)
void pyc_run_module(PyObject* module_name);

void* pyc_alloc(size_t size);
void  pyc_free(void* obj);

// B4/B8 support: apply a callable token (string naming a registered IR function)
// to a Python list of arguments. Returns the result (boxed) or NULL on error.
PyObject* Pyc_Apply(PyObject* token, PyObject* argList);

// B5 (nonlocal/cells): minimal cell primitives.
// A cell is a PyObject with type==6 whose cell_content holds the target PyObject*.
// PyCell_New(initial) allocates a cell holding 'initial' (may be NULL); returns new cell (owned ref).
// PyCell_Get(cell) returns a new reference to the cell's content (or NULL if empty).
// PyCell_Set(cell, val) stores 'val' into the cell (INCREF new, DECREF old if present); returns cell.
PyObject* PyCell_New(PyObject* initial);
PyObject* PyCell_Get(PyObject* cell);
PyObject* PyCell_Set(PyObject* cell, PyObject* val);

// B5 helper: return 1 if obj is a cell (type==6), else 0.
int PyCell_Check(PyObject* obj);

// A7: Allocation counters for measurement and guardrails.
// Returns the current count of allocations for each type.
long PyAlloc_GetIntCount();
long PyAlloc_GetFloatCount();
long PyAlloc_GetListCount();
long PyAlloc_GetDictCount();
long PyAlloc_GetStrCount();
long PyAlloc_GetTotal();

// --- re module (PCRE2-backed) ---------------------------------------------
// re.finditer / re.findall / re.match / re.search / re.sub / re.compile
// return rich objects (lists of Match or compiled regexes).
PyObject* PyBuiltin_ReFinditer(PyObject* pattern, PyObject* subject);
PyObject* PyBuiltin_ReFindall(PyObject* pattern, PyObject* subject);
PyObject* PyBuiltin_ReSearch(PyObject* pattern, PyObject* subject);
PyObject* PyBuiltin_ReCompile(PyObject* pattern);
PyObject* PyBuiltin_ReSub(PyObject* pattern, PyObject* repl, PyObject* subject, PyObject* count);
PyObject* PyBuiltin_ReSplit(PyObject* pattern, PyObject* subject, PyObject* maxsplit);
// m.group(i) — return the i-th capture group as a string.
PyObject* PyBuiltin_ReMatchGroup(PyObject* m, PyObject* idxObj);

// Generator yield helpers (eager materialization):
// pyc_yield_collect(value)  — appends value to thread-local buffer, returns value
// pyc_get_yield_buffer()    — returns collected list, clears buffer
// pyc_clear_yield_buffer()  — clears buffer (called before generator function runs)
PyObject* pyc_yield_collect(PyObject* value);
PyObject* pyc_get_yield_buffer(void);
void      pyc_clear_yield_buffer(void);

#ifdef __cplusplus
}
#endif
