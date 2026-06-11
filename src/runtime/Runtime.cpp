#include <cstdio>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <string>

#include "pyc/runtime.h"

// Flat struct (no union) so we can add dvalue alongside value cleanly.
// LLVM codegen in Codegen.cpp mirrors fields 0-3: {i32, i32, i64, double}.
struct PyObject {
    int refcount;
    int type;   // 0=int, 1=list, 2=dict, 3=str, 4=float, 5=bool
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

// Internal truthiness predicate (mirrors Python's bool()).
static int PyObject_TruthValue(PyObject* obj) {
    if (!obj) return 0;
    if (obj->type == 0 || obj->type == 5) return obj->value != 0;
    if (obj->type == 4) return obj->dvalue != 0.0;
    if (obj->type == 3) return !obj->str.empty();
    if (obj->type == 1) return !obj->list.empty();
    if (obj->type == 2) return !obj->dict.empty();
    return 1;
}

PyObject* PyBool_New(int v) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 5;
    obj->value = v ? 1 : 0;
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

// Boxed wrappers so the compiler can stay entirely in PyObject* world.
PyObject* PyList_SizeBoxed(PyObject* list) {
    return PyInt_FromLong((long)PyList_Size(list));
}

PyObject* PyList_GetItemObj(PyObject* list, PyObject* idx) {
    if (!idx || idx->type != 0) return nullptr;
    return PyList_GetItem(list, (size_t)idx->value);
}

PyObject* PyList_NewBoxed(PyObject* n) {
    size_t size = (n && n->type == 0) ? (size_t)n->value : 0;
    return PyList_New(size);
}

void PyList_SetItemBoxed(PyObject* list, PyObject* idx, PyObject* item) {
    if (!idx || idx->type != 0) return;
    PyList_SetItem(list, (size_t)idx->value, item);
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
    if (obj->type == 5) return fprintf(fp, "%s\n", obj->value ? "True" : "False");
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

// Convert any PyObject to its string representation (no trailing newline).
// Named PyStr_FromAny to avoid conflict with CPython's PyObject_Str.
PyObject* PyStr_FromAny(PyObject* obj) {
    if (!obj) return PyUnicode_FromString("None");
    if (obj->type == 5) return PyUnicode_FromString(obj->value ? "True" : "False");
    if (obj->type == 3) { Py_INCREF(obj); return obj; }
    if (obj->type == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", obj->value);
        return PyUnicode_FromString(buf);
    }
    if (obj->type == 4) {
        char buf[64];
        format_double(buf, sizeof(buf), obj->dvalue);
        return PyUnicode_FromString(buf);
    }
    if (obj->type == 1) {
        std::string r = "[";
        for (size_t i = 0; i < obj->list.size(); ++i) {
            if (i > 0) r += ", ";
            PyObject* s = PyStr_FromAny(obj->list[i]);
            if (obj->list[i] && obj->list[i]->type == 3) { r += "'"; r += obj->list[i]->str; r += "'"; }
            else if (s) r += s->str;
            if (s) Py_DECREF(s);
        }
        r += "]";
        return PyUnicode_FromString(r.c_str());
    }
    return PyUnicode_FromString("<object>");
}

PyObject* PyString_Concat(PyObject* a, PyObject* b) {
    if (!a || !b || a->type != 3 || b->type != 3) return nullptr;
    return PyUnicode_FromString((a->str + b->str).c_str());
}

PyObject* PyString_Repeat(PyObject* s, PyObject* n) {
    if (!s || !n || s->type != 3 || n->type != 0) return nullptr;
    std::string r;
    for (long i = 0; i < n->value; ++i) r += s->str;
    return PyUnicode_FromString(r.c_str());
}

PyObject* PyObject_TruthBoxed(PyObject* obj) {
    return PyBool_New(PyObject_TruthValue(obj));
}

PyObject* PyObject_Not(PyObject* obj) {
    return PyBool_New(!PyObject_TruthValue(obj));
}

PyObject* PyNumber_Negate(PyObject* obj) {
    if (!obj) return NULL;
    if (obj->type == 0 || obj->type == 5) return PyInt_FromLong(-obj->value);
    if (obj->type == 4) return PyFloat_FromDouble(-obj->dvalue);
    return NULL;
}

PyObject* PyBuiltin_Int(PyObject* obj) {
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 0 || obj->type == 5) return PyInt_FromLong(obj->value);
    if (obj->type == 4) return PyInt_FromLong((long)obj->dvalue);
    if (obj->type == 3) {
        try { return PyInt_FromLong(std::stol(obj->str)); } catch (...) {}
    }
    return PyInt_FromLong(0);
}

PyObject* PyBuiltin_Float(PyObject* obj) {
    if (!obj) return PyFloat_FromDouble(0.0);
    if (obj->type == 0 || obj->type == 5) return PyFloat_FromDouble((double)obj->value);
    if (obj->type == 4) { Py_INCREF(obj); return obj; }
    if (obj->type == 3) {
        try { return PyFloat_FromDouble(std::stod(obj->str)); } catch (...) {}
    }
    return PyFloat_FromDouble(0.0);
}

PyObject* PyBuiltin_Abs(PyObject* obj) {
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 0 || obj->type == 5) return PyInt_FromLong(obj->value < 0 ? -obj->value : obj->value);
    if (obj->type == 4) return PyFloat_FromDouble(obj->dvalue < 0.0 ? -obj->dvalue : obj->dvalue);
    return PyInt_FromLong(0);
}

PyObject* PyString_Upper(PyObject* s) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    std::string r = s->str;
    for (char& c : r) c = (char)toupper((unsigned char)c);
    return PyUnicode_FromString(r.c_str());
}

PyObject* PyString_Lower(PyObject* s) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    std::string r = s->str;
    for (char& c : r) c = (char)tolower((unsigned char)c);
    return PyUnicode_FromString(r.c_str());
}

PyObject* PyString_Strip(PyObject* s) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    size_t l = 0, r = s->str.size();
    while (l < r && isspace((unsigned char)s->str[l])) ++l;
    while (r > l && isspace((unsigned char)s->str[r-1])) --r;
    return PyUnicode_FromString(s->str.substr(l, r - l).c_str());
}

PyObject* PyString_Split(PyObject* s, PyObject* sep) {
    PyObject* result = PyList_New(0);
    if (!s || s->type != 3) return result;
    std::string delim = (sep && sep->type == 3) ? sep->str : " ";
    size_t start = 0, pos;
    while ((pos = s->str.find(delim, start)) != std::string::npos) {
        PyList_Append(result, PyUnicode_FromString(s->str.substr(start, pos - start).c_str()));
        start = pos + delim.size();
    }
    PyList_Append(result, PyUnicode_FromString(s->str.substr(start).c_str()));
    return result;
}

PyObject* PyString_SplitWhitespace(PyObject* s) {
    PyObject* result = PyList_New(0);
    if (!s || s->type != 3) return result;
    size_t i = 0, n = s->str.size();
    while (i < n) {
        while (i < n && isspace((unsigned char)s->str[i])) ++i;
        size_t j = i;
        while (j < n && !isspace((unsigned char)s->str[j])) ++j;
        if (j > i) PyList_Append(result, PyUnicode_FromString(s->str.substr(i, j - i).c_str()));
        i = j;
    }
    return result;
}

PyObject* PyString_Join(PyObject* sep, PyObject* iterable) {
    if (!sep || sep->type != 3 || !iterable || iterable->type != 1)
        return PyUnicode_FromString("");
    std::string r;
    for (size_t i = 0; i < iterable->list.size(); ++i) {
        if (i > 0) r += sep->str;
        if (iterable->list[i] && iterable->list[i]->type == 3) r += iterable->list[i]->str;
    }
    return PyUnicode_FromString(r.c_str());
}

PyObject* PyDict_Keys(PyObject* d) {
    PyObject* result = PyList_New(0);
    if (!d || d->type != 2) return result;
    for (auto& pair : d->dict) { Py_INCREF(pair.first); PyList_Append(result, pair.first); }
    return result;
}

PyObject* PyDict_Values(PyObject* d) {
    PyObject* result = PyList_New(0);
    if (!d || d->type != 2) return result;
    for (auto& pair : d->dict) { Py_INCREF(pair.second); PyList_Append(result, pair.second); }
    return result;
}

PyObject* PyDict_Items(PyObject* d) {
    PyObject* result = PyList_New(0);
    if (!d || d->type != 2) return result;
    for (auto& pair : d->dict) {
        PyObject* item = PyList_New(2);
        Py_INCREF(pair.first); Py_INCREF(pair.second);
        PyList_SetItem(item, 0, pair.first);
        PyList_SetItem(item, 1, pair.second);
        PyList_Append(result, item);
    }
    return result;
}

PyObject* PyList_Sort(PyObject* lst) {
    if (!lst || lst->type != 1) return PyInt_FromLong(0);
    std::sort(lst->list.begin(), lst->list.end(), [](PyObject* a, PyObject* b) -> bool {
        if (!a || !b) return false;
        return PyObject_CompareBool(a, b, 2) != 0;  // Lt
    });
    return PyInt_FromLong(0);
}

PyObject* PyList_Pop(PyObject* lst) {
    if (!lst || lst->type != 1 || lst->list.empty()) return nullptr;
    PyObject* item = lst->list.back();
    lst->list.pop_back();
    return item;
}

PyObject* PyBuiltin_PrintNewline(void) {
    printf("\n");
    return PyInt_FromLong(0);
}

PyObject* PyBuiltin_Len(PyObject* obj) {
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 1) return PyInt_FromLong((long)obj->list.size());
    if (obj->type == 3) return PyInt_FromLong((long)obj->str.size());
    if (obj->type == 2) return PyInt_FromLong((long)obj->dict.size());
    return PyInt_FromLong(0);
}

// Helper: get numeric value as double regardless of type (int or float).
static double numeric_val(PyObject* o) {
    if (!o) return 0.0;
    if (o->type == 0 || o->type == 5) return (double)o->value;
    if (o->type == 4) return o->dvalue;
    return 0.0;
}

static int is_numeric(PyObject* o) {
    return o && (o->type == 0 || o->type == 4 || o->type == 5);
}

// True when neither operand is a float — result stays integer.
static int both_integral(PyObject* a, PyObject* b) {
    return a->type != 4 && b->type != 4;
}

PyObject* Pyc_GetItem(PyObject* obj, PyObject* key) {
    if (!obj || !key) return nullptr;
    if (obj->type == 1) return PyList_GetItemObj(obj, key);
    if (obj->type == 2) {
        for (auto& pair : obj->dict)
            if (PyObject_CompareBool(pair.first, key, 0)) return pair.second;
        return nullptr;
    }
    if (obj->type == 3 && (key->type == 0 || key->type == 5)) {
        long idx = key->value;
        if (idx < 0) idx += (long)obj->str.size();
        if (idx >= 0 && (size_t)idx < obj->str.size()) {
            char buf[2] = {obj->str[(size_t)idx], '\0'};
            return PyUnicode_FromString(buf);
        }
    }
    return nullptr;
}

PyObject* Pyc_SetItem(PyObject* obj, PyObject* key, PyObject* val) {
    if (!obj || !key) return nullptr;
    if (obj->type == 1) { PyList_SetItemBoxed(obj, key, val); return nullptr; }
    if (obj->type == 2) { PyDict_SetItem(obj, key, val); return nullptr; }
    return nullptr;
}

PyObject* Pyc_Contains(PyObject* container, PyObject* item) {
    if (!container || !item) return PyBool_New(0);
    if (container->type == 1) {
        for (auto* elem : container->list)
            if (elem && PyObject_CompareBool(elem, item, 0)) return PyBool_New(1);
        return PyBool_New(0);
    }
    if (container->type == 3) {
        if (item->type == 3)
            return PyBool_New(container->str.find(item->str) != std::string::npos);
        return PyBool_New(0);
    }
    if (container->type == 2) {
        for (auto& pair : container->dict)
            if (PyObject_CompareBool(pair.first, item, 0)) return PyBool_New(1);
        return PyBool_New(0);
    }
    return PyBool_New(0);
}

PyObject* Pyc_Pow(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return nullptr;
    if (both_integral(a, b) && b->value >= 0) {
        long result = 1, base = a->value, exp = b->value;
        for (long i = 0; i < exp; ++i) result *= base;
        return PyInt_FromLong(result);
    }
    return PyFloat_FromDouble(pow(numeric_val(a), numeric_val(b)));
}

PyObject* PyBuiltin_Sum(PyObject* lst) {
    if (!lst || lst->type != 1) return PyInt_FromLong(0);
    PyObject* total = PyInt_FromLong(0);
    for (auto* item : lst->list) {
        if (!item) continue;
        PyObject* next = PyNumber_Add(total, item);
        Py_DECREF(total);
        total = next ? next : PyInt_FromLong(0);
    }
    return total;
}

PyObject* PyBuiltin_Sorted(PyObject* lst) {
    if (!lst || lst->type != 1) return PyList_New(0);
    PyObject* r = PyList_New(lst->list.size());
    for (size_t i = 0; i < lst->list.size(); ++i) {
        if (lst->list[i]) Py_INCREF(lst->list[i]);
        PyList_SetItem(r, i, lst->list[i]);
    }
    std::sort(r->list.begin(), r->list.end(), [](PyObject* a, PyObject* b) -> bool {
        if (!a || !b) return false;
        return PyObject_CompareBool(a, b, 2) != 0;
    });
    return r;
}

PyObject* PyBuiltin_Any(PyObject* lst) {
    if (!lst || lst->type != 1) return PyBool_New(0);
    for (auto* item : lst->list)
        if (PyObject_TruthValue(item)) return PyBool_New(1);
    return PyBool_New(0);
}

PyObject* PyBuiltin_All(PyObject* lst) {
    if (!lst || lst->type != 1) return PyBool_New(1);
    for (auto* item : lst->list)
        if (!PyObject_TruthValue(item)) return PyBool_New(0);
    return PyBool_New(1);
}

// typecode: 0=int, 1=list, 2=dict, 3=str, 4=float, 5=bool; -1=unknown→True
PyObject* Pyc_IsInstance(PyObject* obj, PyObject* typecode) {
    if (!obj) return PyBool_New(0);
    if (!typecode || typecode->type != 0 || typecode->value < 0)
        return PyBool_New(1);
    int code = (int)typecode->value;
    bool ok = (obj->type == code) ||
              (code == 0 && obj->type == 5);  // bool is-a int
    return PyBool_New(ok ? 1 : 0);
}

PyObject* PyString_Find(PyObject* s, PyObject* sub) {
    if (!s || s->type != 3 || !sub || sub->type != 3)
        return PyInt_FromLong(-1);
    size_t pos = s->str.find(sub->str);
    return PyInt_FromLong(pos == std::string::npos ? -1L : (long)pos);
}

PyObject* PyString_Count(PyObject* s, PyObject* sub) {
    if (!s || s->type != 3 || !sub || sub->type != 3 || sub->str.empty())
        return PyInt_FromLong(0);
    long count = 0;
    size_t pos = 0;
    while ((pos = s->str.find(sub->str, pos)) != std::string::npos) {
        ++count; pos += sub->str.size();
    }
    return PyInt_FromLong(count);
}

PyObject* PyString_Replace(PyObject* s, PyObject* old_, PyObject* new_) {
    if (!s || s->type != 3 || !old_ || old_->type != 3 || !new_ || new_->type != 3)
        return s ? (Py_INCREF(s), s) : nullptr;
    std::string result = s->str;
    const std::string& from = old_->str;
    const std::string& to   = new_->str;
    if (from.empty()) return PyUnicode_FromString(result.c_str());
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.size(), to);
        pos += to.size();
    }
    return PyUnicode_FromString(result.c_str());
}

PyObject* Pyc_GetSlice(PyObject* obj, PyObject* start, PyObject* stop) {
    if (!obj) return PyList_New(0);
    long n = (obj->type == 1) ? (long)obj->list.size()
           : (obj->type == 3) ? (long)obj->str.size() : 0;
    long s = (start && (start->type==0||start->type==5)) ? start->value : 0;
    long e = (stop  && (stop->type ==0||stop->type ==5) && stop->value >= 0)
             ? stop->value : n;
    if (s < 0) s = std::max(0L, n + s);
    if (e < 0) e = std::max(0L, n + e);
    if (s > n) s = n;
    if (e > n) e = n;
    if (e < s) e = s;
    if (obj->type == 1) {
        PyObject* r = PyList_New(e - s);
        for (long i = s; i < e; ++i) {
            if (obj->list[i]) Py_INCREF(obj->list[i]);
            PyList_SetItem(r, i - s, obj->list[i]);
        }
        return r;
    }
    if (obj->type == 3)
        return PyUnicode_FromString(obj->str.substr(s, e - s).c_str());
    return PyList_New(0);
}

PyObject* PyBuiltin_Min2(PyObject* a, PyObject* b) {
    if (!a) return (b ? (Py_INCREF(b), b) : nullptr);
    if (!b) return (Py_INCREF(a), a);
    return PyObject_CompareBool(a, b, 2) ? (Py_INCREF(a), a) : (Py_INCREF(b), b);
}
PyObject* PyBuiltin_Max2(PyObject* a, PyObject* b) {
    if (!a) return (b ? (Py_INCREF(b), b) : nullptr);
    if (!b) return (Py_INCREF(a), a);
    return PyObject_CompareBool(a, b, 3) ? (Py_INCREF(a), a) : (Py_INCREF(b), b);
}
PyObject* PyBuiltin_MinList(PyObject* lst) {
    if (!lst || lst->type != 1 || lst->list.empty()) return nullptr;
    PyObject* r = lst->list[0];
    for (size_t i = 1; i < lst->list.size(); ++i)
        if (lst->list[i] && PyObject_CompareBool(lst->list[i], r, 2)) r = lst->list[i];
    Py_INCREF(r); return r;
}
PyObject* PyBuiltin_MaxList(PyObject* lst) {
    if (!lst || lst->type != 1 || lst->list.empty()) return nullptr;
    PyObject* r = lst->list[0];
    for (size_t i = 1; i < lst->list.size(); ++i)
        if (lst->list[i] && PyObject_CompareBool(lst->list[i], r, 3)) r = lst->list[i];
    Py_INCREF(r); return r;
}
PyObject* PyBuiltin_List(PyObject* obj) {
    if (!obj) return PyList_New(0);
    if (obj->type == 1) { Py_INCREF(obj); return obj; }
    if (obj->type == 3) {
        PyObject* r = PyList_New(obj->str.size());
        for (size_t i = 0; i < obj->str.size(); ++i) {
            char buf[2] = {obj->str[i], '\0'};
            PyList_SetItem(r, i, PyUnicode_FromString(buf));
        }
        return r;
    }
    return PyList_New(0);
}
PyObject* PyBuiltin_Enumerate(PyObject* iterable) {
    if (!iterable || iterable->type != 1) return PyList_New(0);
    PyObject* r = PyList_New(iterable->list.size());
    for (size_t i = 0; i < iterable->list.size(); ++i) {
        PyObject* pair = PyList_New(2);
        PyList_SetItem(pair, 0, PyInt_FromLong((long)i));
        PyObject* v = iterable->list[i];
        if (v) Py_INCREF(v);
        PyList_SetItem(pair, 1, v);
        PyList_SetItem(r, i, pair);
    }
    return r;
}
PyObject* PyBuiltin_Zip2(PyObject* a, PyObject* b) {
    if (!a || !b) return PyList_New(0);
    size_t na = (a->type == 1) ? a->list.size() : 0;
    size_t nb = (b->type == 1) ? b->list.size() : 0;
    size_t n = na < nb ? na : nb;
    PyObject* r = PyList_New(n);
    for (size_t i = 0; i < n; ++i) {
        PyObject* pair = PyList_New(2);
        PyObject* va = a->list[i]; if (va) Py_INCREF(va); PyList_SetItem(pair, 0, va);
        PyObject* vb = b->list[i]; if (vb) Py_INCREF(vb); PyList_SetItem(pair, 1, vb);
        PyList_SetItem(r, i, pair);
    }
    return r;
}

// str % val formatting (used via PyNumber_Remainder for string left operand)
static PyObject* PyString_Format(PyObject* fmt, PyObject* args) {
    if (!fmt || fmt->type != 3) return nullptr;
    auto getArg = [&](size_t idx) -> PyObject* {
        if (!args) return nullptr;
        if (args->type == 1 && idx < args->list.size()) return args->list[idx];
        return (idx == 0) ? args : nullptr;
    };
    std::string result;
    const std::string& f = fmt->str;
    size_t argIdx = 0;
    for (size_t i = 0; i < f.size(); ) {
        if (f[i] != '%') { result += f[i++]; continue; }
        if (i + 1 >= f.size() || f[i+1] == '%') { result += '%'; i += (f[i+1]=='%' ? 2 : 1); continue; }
        // Find end of format spec
        size_t j = i + 1;
        while (j < f.size() && (f[j]=='-'||f[j]=='+'||f[j]==' '||f[j]=='0'||f[j]=='#'||f[j]=='.'||isdigit((unsigned char)f[j]))) ++j;
        if (j >= f.size()) { result += f[i++]; continue; }
        char spec = f[j];
        PyObject* arg = getArg(argIdx++);
        char buf[256] = {};
        std::string fs = f.substr(i, j - i + 1);  // full %...spec
        if (spec == 'd' || spec == 'i') {
            long v = arg ? ((arg->type==0||arg->type==5) ? arg->value : (arg->type==4 ? (long)arg->dvalue : 0)) : 0;
            std::string lfs = f.substr(i, j-i) + "ld";
            snprintf(buf, sizeof(buf), lfs.c_str(), v);
        } else if (spec == 'f' || spec == 'e' || spec == 'g') {
            double v = arg ? numeric_val(arg) : 0.0;
            snprintf(buf, sizeof(buf), fs.c_str(), v);
        } else if (spec == 's' || spec == 'r') {
            PyObject* s = arg ? PyStr_FromAny(arg) : PyUnicode_FromString("");
            snprintf(buf, sizeof(buf), "%s", s ? s->str.c_str() : "");
            if (s) Py_DECREF(s);
        } else {
            result += fs; i = j + 1; continue;
        }
        result += buf;
        i = j + 1;
    }
    return PyUnicode_FromString(result.c_str());
}

PyObject* PyNumber_Add(PyObject* a, PyObject* b) {
    if (!a || !b) return NULL;
    if (a->type == 3 && b->type == 3) return PyString_Concat(a, b);
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) return PyInt_FromLong(a->value + b->value);
    return PyFloat_FromDouble(numeric_val(a) + numeric_val(b));
}

PyObject* PyNumber_Subtract(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) return PyInt_FromLong(a->value - b->value);
    return PyFloat_FromDouble(numeric_val(a) - numeric_val(b));
}

PyObject* PyNumber_Multiply(PyObject* a, PyObject* b) {
    if (!a || !b) return NULL;
    if (a->type == 3 && b->type == 0) return PyString_Repeat(a, b);
    if (a->type == 0 && b->type == 3) return PyString_Repeat(b, a);
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) return PyInt_FromLong(a->value * b->value);
    return PyFloat_FromDouble(numeric_val(a) * numeric_val(b));
}

// Floor division (//)
PyObject* PyNumber_Divide(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) {
        if (b->value == 0) return NULL;
        long q = a->value / b->value;
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
    if (a && a->type == 3) return PyString_Format(a, b);   // "fmt" % val
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) {
        if (b->value == 0) return NULL;
        long r = a->value % b->value;
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
    // String comparison
    if (a->type == 3 && b->type == 3) {
        int cmp = a->str.compare(b->str);
        switch (op) {
            case 0: return cmp == 0;
            case 1: return cmp != 0;
            case 2: return cmp <  0;
            case 3: return cmp >  0;
            case 4: return cmp <= 0;
            case 5: return cmp >= 0;
        }
    }
    // Pointer equality fallback
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
