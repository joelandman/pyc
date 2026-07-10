#pragma once

#include <stdio.h>

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
void      PyList_SetItemBoxed(PyObject* list, PyObject* idx, PyObject* item);
PyObject* PyList_Comprehension(int start, int end);
PyObject* PyFloat_FromDouble(double v);
PyObject* PyNumber_TrueDivide(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Range(PyObject* start, PyObject* stop, PyObject* step);
int       PyObject_CompareBool(PyObject* a, PyObject* b, int op);
PyObject* PyStr_FromAny(PyObject* obj);
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
PyObject* Pyc_SetItem(PyObject* obj, PyObject* key, PyObject* val);
PyObject* Pyc_Contains(PyObject* container, PyObject* item);
PyObject* Pyc_Pow(PyObject* a, PyObject* b);
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
PyObject* pyc_current_exception(void);
void      pyc_clear_exception(void);

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

#ifdef __cplusplus
}
#endif
