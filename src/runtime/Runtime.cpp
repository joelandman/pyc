#include <cstdio>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <unordered_map>
#include <string>

#include "pyc/runtime.h"

// Internal definition - users should only use the opaque type from runtime.h
struct PyObject {
    int refcount;
    int type;   // 0 = int, 1 = list, 2 = dict, 3 = str
    union {
        long value;
        std::vector<PyObject*> list;
        std::unordered_map<PyObject*, PyObject*> dict;
    };
    std::string str;
};

void Py_INCREF(PyObject* obj) {
    if (obj) ++obj->refcount;
}

extern "C" {

PyObject* PyInt_FromLong(long v) {
    PyObject* obj = (PyObject*)malloc(sizeof(PyObject));
    obj->refcount = 1;
    obj->type = 0;
    obj->value = v;
    return obj;
}

PyObject* PyList_New(size_t size) {
    PyObject* obj = (PyObject*)malloc(sizeof(PyObject));
    obj->refcount = 1;
    obj->type = 1;
    obj->list = std::vector<PyObject*>(size, nullptr);
    return obj;
}

PyObject* PyList_GetItem(PyObject* list, size_t index) {
    if (list && list->type == 1 && index < list->list.size()) {
        return list->list[index];
    }
    return nullptr;
}

size_t PyList_Size(PyObject* list) {
    if (list && list->type == 1) {
        return list->list.size();
    }
    return 0;
}

void PyList_SetItem(PyObject* list, size_t index, PyObject* item) {
    if (list && list->type == 1 && index < list->list.size()) {
        // Decrement reference count of old item if it exists
        if (list->list[index]) {
            Py_DECREF(list->list[index]);
        }
        list->list[index] = item;
        if (item) Py_INCREF(item);
    }
}

PyObject* PyList_Append(PyObject* list, PyObject* item) {
    if (list && list->type == 1) {
        list->list.push_back(item);
        if (item) Py_INCREF(item);
        return list;
    }
    return nullptr;
}

PyObject* PyList_FromArray(PyObject** items, size_t size) {
    PyObject* obj = PyList_New(size);
    for (size_t i = 0; i < size; ++i) {
        PyList_SetItem(obj, i, items[i]);
    }
    return obj;
}

// Helper function to create a simple range-like list for testing
PyObject* PyList_Range(int start, int end) {
    PyObject* list = PyList_New(end - start);
    for (int i = start; i < end; i++) {
        PyObject* item = PyInt_FromLong(i);
        PyList_SetItem(list, i - start, item);
    }
    return list;
}

// Helper function to create a list comprehension result for testing
PyObject* PyList_Comprehension(int start, int end) {
    PyObject* list = PyList_New(end - start);
    for (int i = start; i < end; i++) {
        PyObject* item = PyInt_FromLong(i);
        PyList_SetItem(list, i - start, item);
    }
    return list;
}

PyObject* PyDict_New() {
    PyObject* obj = (PyObject*)malloc(sizeof(PyObject));
    obj->refcount = 1;
    obj->type = 2;
    obj->dict = std::unordered_map<PyObject*, PyObject*>();
    return obj;
}

void PyDict_SetItem(PyObject* dict, PyObject* key, PyObject* value) {
    if (dict && dict->type == 2) {
        // For simplicity, we'll use the key as the map key directly
        // In practice, this would need proper hash and equality comparison
        dict->dict[key] = value;
        if (value) Py_INCREF(value);
    }
}

PyObject* PyDict_GetItem(PyObject* dict, PyObject* key) {
    if (dict && dict->type == 2) {
        auto it = dict->dict.find(key);
        if (it != dict->dict.end()) {
            return it->second;
        }
    }
    return nullptr;
}

// Helper function for dictionary comprehension
PyObject* PyDict_Comprehension(int start, int end) {
    PyObject* dict = PyDict_New();
    for (int i = start; i < end; i++) {
        PyObject* key = PyInt_FromLong(i);
        PyObject* value = PyInt_FromLong(i * i);
        PyDict_SetItem(dict, key, value);
        Py_DECREF(key);
        Py_DECREF(value);
    }
    return dict;
}

void Py_DECREF(PyObject* obj) {
    if (obj && --obj->refcount == 0) {
        if (obj->type == 1) {
            // Free list elements
            for (PyObject* item : obj->list) {
                Py_DECREF(item);
            }
        } else if (obj->type == 2) {
            // Free dict elements
            for (auto& pair : obj->dict) {
                Py_DECREF(pair.first);
                Py_DECREF(pair.second);
            }
        }
        free(obj);
    }
}

int PyObject_Print(PyObject* obj, FILE* fp) {
    if (!fp) fp = stdout;   // tolerate null FILE* during boxed transition
    if (obj && obj->type == 0) {
        return fprintf(fp, "%ld\n", obj->value);
    } else if (obj && obj->type == 1) {
        fprintf(fp, "[");
        for (size_t i = 0; i < obj->list.size(); ++i) {
            if (i > 0) fprintf(fp, ", ");
            PyObject_Print(obj->list[i], fp);
        }
        fprintf(fp, "]\n");
        return 0;
    } else if (obj && obj->type == 2) {
        fprintf(fp, "{");
        bool first = true;
        for (auto& pair : obj->dict) {
            if (!first) fprintf(fp, ", ");
            PyObject_Print(pair.first, fp);
            fprintf(fp, ": ");
            PyObject_Print(pair.second, fp);
            first = false;
        }
        fprintf(fp, "}\n");
        return 0;
    } else if (obj && obj->type == 3) {
        return fprintf(fp, "%s\n", obj->str.c_str());
    }
    return fprintf(fp, "<object>\n");
}

PyObject* PyUnicode_FromString(const char* s) {
    PyObject* obj = (PyObject*)malloc(sizeof(PyObject));
    obj->refcount = 1;
    obj->type = 3;
    obj->str = s ? s : "";
    return obj;
}

PyObject* PyNumber_Add(PyObject* a, PyObject* b) {
    if (a && b && a->type == 0 && b->type == 0) {
        return PyInt_FromLong(a->value + b->value);
    }
    return NULL;
}

PyObject* PyNumber_Multiply(PyObject* a, PyObject* b) {
    if (a && b && a->type == 0 && b->type == 0) {
        return PyInt_FromLong(a->value * b->value);
    }
    return NULL;
}

PyObject* PyNumber_Subtract(PyObject* a, PyObject* b) {
    if (a && b && a->type == 0 && b->type == 0) {
        return PyInt_FromLong(a->value - b->value);
    }
    return NULL;
}

PyObject* PyNumber_Divide(PyObject* a, PyObject* b) {
    if (a && b && a->type == 0 && b->type == 0 && b->value != 0) {
        return PyInt_FromLong(a->value / b->value);
    }
    return NULL;
}

PyObject* PyNumber_Remainder(PyObject* a, PyObject* b) {
    if (a && b && a->type == 0 && b->type == 0 && b->value != 0) {
        return PyInt_FromLong(a->value % b->value);
    }
    return NULL;
}

PyObject* PyObject_GetAttr(PyObject* obj, const char* attr) {
    // Minimal implementation: return the object itself for now
    if (obj) Py_INCREF(obj);
    return obj;
}

void PyErr_Print(void) {
    fprintf(stderr, "Python error occurred\n");
}

void* pyc_alloc(size_t size) { return malloc(size); }
void pyc_free(void* obj) { free(obj); }

}

 // No main (provided by generated code); link as library of builtins
