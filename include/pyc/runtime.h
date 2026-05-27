#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PyObject PyObject;

PyObject* PyInt_FromLong(long v);
void      Py_DECREF(PyObject* obj);
int       PyObject_Print(PyObject* obj, FILE* fp);
PyObject* PyNumber_Add(PyObject* a, PyObject* b);
void      PyErr_Print(void);

void* pyc_alloc(size_t size);
void  pyc_free(void* obj);

#ifdef __cplusplus
}
#endif
