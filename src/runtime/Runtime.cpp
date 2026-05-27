#include <cstdio>
#include <stdio.h>
#include <stdlib.h>

#include "pyc/runtime.h"

// Internal definition - users should only use the opaque type from runtime.h
struct PyObject {
    int refcount;
    int type;   // 0 = int
    long value;
};

extern "C" {

PyObject* PyInt_FromLong(long v) {
    PyObject* obj = (PyObject*)malloc(sizeof(PyObject));
    obj->refcount = 1;
    obj->type = 0;
    obj->value = v;
    return obj;
}

void Py_DECREF(PyObject* obj) {
    if (obj && --obj->refcount == 0) {
        free(obj);
    }
}

void Py_INCREF(PyObject* obj) {
    if (obj) ++obj->refcount;
}

int PyObject_Print(PyObject* obj, FILE* fp) {
    if (!fp) fp = stdout;   // tolerate null FILE* during boxed transition
    if (obj && obj->type == 0) {
        return fprintf(fp, "%ld\n", obj->value);
    }
    return fprintf(fp, "<object>\n");
}

PyObject* PyNumber_Add(PyObject* a, PyObject* b) {
    if (a && b && a->type == 0 && b->type == 0) {
        return PyInt_FromLong(a->value + b->value);
    }
    return NULL;
}

void PyErr_Print(void) {
    fprintf(stderr, "Python error occurred\n");
}

void* pyc_alloc(size_t size) { return malloc(size); }
void pyc_free(void* obj) { free(obj); }

}

 // No main (provided by generated code); link as library of builtins
