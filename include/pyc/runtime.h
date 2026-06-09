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
PyObject* PyBuiltin_PrintNewline(void);
PyObject* PyBool_New(int v);
PyObject* PyBuiltin_Min2(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Max2(PyObject* a, PyObject* b);
PyObject* PyBuiltin_MinList(PyObject* lst);
PyObject* PyBuiltin_MaxList(PyObject* lst);
PyObject* PyBuiltin_List(PyObject* obj);
PyObject* PyBuiltin_Enumerate(PyObject* iterable);
PyObject* PyBuiltin_Zip2(PyObject* a, PyObject* b);
PyObject* PyBuiltin_Int(PyObject* obj);
PyObject* PyBuiltin_Float(PyObject* obj);
PyObject* PyBuiltin_Abs(PyObject* obj);
PyObject* PyString_Upper(PyObject* s);
PyObject* PyString_Lower(PyObject* s);
PyObject* PyString_Strip(PyObject* s);
PyObject* PyString_Split(PyObject* s, PyObject* sep);
PyObject* PyString_SplitWhitespace(PyObject* s);
PyObject* PyString_Join(PyObject* sep, PyObject* iterable);
PyObject* PyDict_Keys(PyObject* d);
PyObject* PyDict_Values(PyObject* d);
PyObject* PyDict_Items(PyObject* d);
PyObject* PyList_Sort(PyObject* lst);
PyObject* PyList_Pop(PyObject* lst);
PyObject* PyObject_TruthBoxed(PyObject* obj);
PyObject* Pyc_GetItem(PyObject* obj, PyObject* key);
PyObject* Pyc_SetItem(PyObject* obj, PyObject* key, PyObject* val);
PyObject* Pyc_Contains(PyObject* container, PyObject* item);
PyObject* Pyc_Pow(PyObject* a, PyObject* b);
PyObject* PyObject_Not(PyObject* obj);
PyObject* PyNumber_Negate(PyObject* obj);
void      PyErr_Print(void);

void* pyc_alloc(size_t size);
void  pyc_free(void* obj);

#ifdef __cplusplus
}
#endif
