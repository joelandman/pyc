#include <cstdio>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <unordered_map>
#include <string>

#include "pyc/runtime.h"

// Flat struct (no union) so we can add dvalue alongside value cleanly.
// LLVM codegen in Codegen.cpp mirrors fields 0-3: {i32, i32, i64, double}.
struct PyObject {
    int refcount;
    int type;   // 0=int, 1=list, 2=dict, 3=str, 4=float
    long value;    // type 0
    double dvalue; // type 4
    std::vector<PyObject*> list;
    std::unordered_map<PyObject*, PyObject*> dict;
    std::string str;
};

void Py_INCREF(PyObject* obj) {
    if (obj) ++obj->refcount;
}

// Shortest round-trip decimal representation, always with a decimal point.
static void format_double(char* buf, size_t bufsize, double v) {
    if (v != v)          { snprintf(buf, bufsize, "nan");  return; }
    if (v ==  1.0/0.0)   { snprintf(buf, bufsize, "inf");  return; }
    if (v == -1.0/0.0)   { snprintf(buf, bufsize, "-inf"); return; }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, bufsize, "%.*g", prec, v);
        double check;
        if (sscanf(buf, "%lf", &check) == 1 && check == v) break;
    }
    // Guarantee at least one decimal digit so it reads as float, not int.
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&
        !strchr(buf, 'n') && !strchr(buf, 'i')) {
        size_t len = strlen(buf);
        if (len + 2 < bufsize) { buf[len] = '.'; buf[len+1] = '0'; buf[len+2] = '\0'; }
    }
}

extern "C" {

PyObject* PyInt_FromLong(long v) {
    PyObject* obj = new PyObject();   // calls ctors for vector/map/string
    obj->refcount = 1;
    obj->type = 0;
    obj->value = v;
    return obj;
}

PyObject* PyFloat_FromDouble(double v) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 4;
    obj->dvalue = v;
    return obj;
}

PyObject* PyList_New(size_t size) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 1;
    obj->list.assign(size, nullptr);
    return obj;
}

PyObject* PyList_GetItem(PyObject* list, size_t index) {
    if (list && list->type == 1 && index < list->list.size())
        return list->list[index];
    return nullptr;
}

size_t PyList_Size(PyObject* list) {
    if (list && list->type == 1)
        return list->list.size();
    return 0;
}

void PyList_SetItem(PyObject* list, size_t index, PyObject* item) {
    if (list && list->type == 1 && index < list->list.size()) {
        if (list->list[index]) Py_DECREF(list->list[index]);
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
    for (size_t i = 0; i < size; ++i) PyList_SetItem(obj, i, items[i]);
    return obj;
}

// Boxed wrappers so for-loops can stay entirely in PyObject* world.
PyObject* PyList_SizeBoxed(PyObject* list) {
    return PyInt_FromLong((long)PyList_Size(list));
}

PyObject* PyList_GetItemObj(PyObject* list, PyObject* idx) {
    if (!idx || idx->type != 0) return nullptr;
    return PyList_GetItem(list, (size_t)idx->value);
}

PyObject* PyList_Range(int start, int end) {
    PyObject* list = PyList_New(end > start ? end - start : 0);
    for (int i = start; i < end; i++)
        PyList_SetItem(list, i - start, PyInt_FromLong(i));
    return list;
}

PyObject* PyList_Comprehension(int start, int end) {
    return PyList_Range(start, end);
}

PyObject* PyBuiltin_Range(PyObject* start, PyObject* stop, PyObject* step) {
    long s  = (start && start->type == 0) ? start->value : 0;
    long e  = (stop  && stop->type  == 0) ? stop->value  : 0;
    long st = (step  && step->type  == 0) ? step->value  : 1;
    if (st == 0) return PyList_New(0);

    long count = 0;
    for (long i = s; st > 0 ? i < e : i > e; i += st) count++;

    PyObject* list = PyList_New((size_t)count);
    long idx = 0;
    for (long i = s; st > 0 ? i < e : i > e; i += st)
        PyList_SetItem(list, idx++, PyInt_FromLong(i));
    return list;
}

PyObject* PyDict_New() {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 2;
    return obj;
}

void PyDict_SetItem(PyObject* dict, PyObject* key, PyObject* value) {
    if (dict && dict->type == 2) {
        dict->dict[key] = value;
        if (value) Py_INCREF(value);
    }
}

PyObject* PyDict_GetItem(PyObject* dict, PyObject* key) {
    if (dict && dict->type == 2) {
        auto it = dict->dict.find(key);
        if (it != dict->dict.end()) return it->second;
    }
    return nullptr;
}

void Py_DECREF(PyObject* obj) {
    if (obj && --obj->refcount == 0) {
        if (obj->type == 1) {
            for (PyObject* item : obj->list) if (item) Py_DECREF(item);
        } else if (obj->type == 2) {
            for (auto& pair : obj->dict) {
                Py_DECREF(pair.first);
                Py_DECREF(pair.second);
            }
        }
        delete obj;   // calls dtors for vector/map/string
    }
}

int PyObject_Print(PyObject* obj, FILE* fp) {
    if (!fp) fp = stdout;
    if (!obj) return fprintf(fp, "None\n");
    if (obj->type == 0) return fprintf(fp, "%ld\n", obj->value);
    if (obj->type == 4) {
        char buf[64];
        format_double(buf, sizeof(buf), obj->dvalue);
        return fprintf(fp, "%s\n", buf);
    }
    if (obj->type == 1) {
        fprintf(fp, "[");
        for (size_t i = 0; i < obj->list.size(); ++i) {
            if (i > 0) fprintf(fp, ", ");
            if (obj->list[i] && obj->list[i]->type == 3)
                fprintf(fp, "'%s'", obj->list[i]->str.c_str());
            else
                PyObject_Print(obj->list[i], stdout);  // recurse without newline — TODO
        }
        fprintf(fp, "]\n");
        return 0;
    }
    if (obj->type == 2) {
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
    }
    if (obj->type == 3) return fprintf(fp, "%s\n", obj->str.c_str());
    return fprintf(fp, "<object>\n");
}

PyObject* PyUnicode_FromString(const char* s) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 3;
    obj->str = s ? s : "";
    return obj;
}

// Helper: get numeric value as double regardless of type (int or float).
static double numeric_val(PyObject* o) {
    if (!o) return 0.0;
    if (o->type == 0) return (double)o->value;
    if (o->type == 4) return o->dvalue;
    return 0.0;
}

static int is_numeric(PyObject* o) {
    return o && (o->type == 0 || o->type == 4);
}

PyObject* PyNumber_Add(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (a->type == 0 && b->type == 0) return PyInt_FromLong(a->value + b->value);
    return PyFloat_FromDouble(numeric_val(a) + numeric_val(b));
}

PyObject* PyNumber_Subtract(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (a->type == 0 && b->type == 0) return PyInt_FromLong(a->value - b->value);
    return PyFloat_FromDouble(numeric_val(a) - numeric_val(b));
}

PyObject* PyNumber_Multiply(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (a->type == 0 && b->type == 0) return PyInt_FromLong(a->value * b->value);
    return PyFloat_FromDouble(numeric_val(a) * numeric_val(b));
}

// Floor division (//)
PyObject* PyNumber_Divide(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (a->type == 0 && b->type == 0) {
        if (b->value == 0) return NULL;
        long q = a->value / b->value;
        // Python truncates toward negative infinity
        if ((a->value ^ b->value) < 0 && q * b->value != a->value) q--;
        return PyInt_FromLong(q);
    }
    double bv = numeric_val(b);
    if (bv == 0.0) return NULL;
    return PyFloat_FromDouble(floor(numeric_val(a) / bv));
}

// True division (/)
PyObject* PyNumber_TrueDivide(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    double bv = numeric_val(b);
    if (bv == 0.0) return NULL;
    return PyFloat_FromDouble(numeric_val(a) / bv);
}

PyObject* PyNumber_Remainder(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (a->type == 0 && b->type == 0) {
        if (b->value == 0) return NULL;
        long r = a->value % b->value;
        // Python modulo sign matches divisor
        if (r != 0 && (r ^ b->value) < 0) r += b->value;
        return PyInt_FromLong(r);
    }
    double bv = numeric_val(b);
    if (bv == 0.0) return NULL;
    double r = fmod(numeric_val(a), bv);
    if (r != 0.0 && ((r < 0) != (bv < 0))) r += bv;
    return PyFloat_FromDouble(r);
}

// PyObject_CompareBool: op codes match Codegen.cpp icmp dispatch
// 0=Eq, 1=NotEq, 2=Lt, 3=Gt, 4=LtE, 5=GtE
int PyObject_CompareBool(PyObject* a, PyObject* b, int op) {
    if (!a || !b) return 0;
    // Numeric comparison (int or float)
    if (is_numeric(a) && is_numeric(b)) {
        double av = numeric_val(a);
        double bv = numeric_val(b);
        switch (op) {
            case 0: return av == bv;
            case 1: return av != bv;
            case 2: return av <  bv;
            case 3: return av >  bv;
            case 4: return av <= bv;
            case 5: return av >= bv;
        }
    }
    // Pointer equality fallback for non-numeric types
    switch (op) {
        case 0: return a == b;
        case 1: return a != b;
        default: return 0;
    }
}

PyObject* PyObject_GetAttr(PyObject* obj, const char* attr) {
    if (obj) Py_INCREF(obj);
    return obj;
}

void PyErr_Print(void) { fprintf(stderr, "Python error occurred\n"); }

// Comprehension helpers (kept for backward compat)
PyObject* list_create() { return PyList_New(0); }
void list_append(PyObject* list, PyObject* item) { PyList_Append(list, item); }
PyObject* dict_create() { return PyDict_New(); }
void dict_add(PyObject* dict, PyObject* key, PyObject* value) { PyDict_SetItem(dict, key, value); }
PyObject* iter_create(PyObject* iterable) { Py_INCREF(iterable); return iterable; }
int iter_has_next(PyObject*) { return 1; }
PyObject* iter_next(PyObject*) { return PyInt_FromLong(0); }

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

void* pyc_alloc(size_t size) { return ::operator new(size); }
void  pyc_free(void* obj)    { ::operator delete(obj); }

} // extern "C"
