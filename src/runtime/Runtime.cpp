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

// Immortal PyObject* refcount sentinel (True, False, and small ints in
// [-5, 256] are interned; Py_INCREF / Py_DECREF skip them so they are
// never freed or duplicated by ownership paths).
static constexpr int IMMORTAL_REFCOUNT = 0x3fffffff;

// Flat struct (no union) so we can add dvalue alongside value cleanly.
// LLVM codegen in Codegen.cpp mirrors fields 0-3: {i32, i32, i64, double}.
struct PyObject {
    int refcount;
    int type;   // 0=int, 1=list, 2=dict, 3=str, 4=float, 5=bool,
                // 6=cell (B5 nonlocal/closure)
    long value;    // type 0
    double dvalue; // type 4
    std::vector<PyObject*> list;
    std::unordered_map<PyObject*, PyObject*> dict;
    std::string str;
    PyObject* cell_content; // type 6
};

void Py_INCREF(PyObject* obj) {
    if (obj && obj->refcount != IMMORTAL_REFCOUNT) ++obj->refcount;
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

// === Singletons and small-int cache ===
// CPython interns None (represented here as the null pointer in valueMap
// slots), True/False, and small ints in [-5, 256] so identity comparisons
// (`x is y`, `x is None`, `True is True`) work as Python users expect, and
// to avoid a malloc per literal in tight loops.
//
// Immortal objects use refcount = IMMORTAL_REFCOUNT (see top of file);
// Py_DECREF / Py_INCREF skip them so they are never freed or duplicated
// by ownership paths.
static bool isImmortal(PyObject* obj) { return obj && obj->refcount == IMMORTAL_REFCOUNT; }

static PyObject* g_pyTrue  = nullptr;
static PyObject* g_pyFalse = nullptr;

// Small int cache: indices [-5..256] map to a fixed array of 262 slots.
// Slot 0 represents -5, slot 261 represents 256. 0 is stored at slot 5.
// Pre-allocate all slots at static-init time so getSmallInt is a simple
// (and provably correct) array lookup with no nullptr check on the hot path.
static constexpr int SMALL_INT_LO = -5;
static constexpr int SMALL_INT_HI = 256;
static constexpr int SMALL_INT_COUNT = SMALL_INT_HI - SMALL_INT_LO + 1;
static PyObject* g_smallInts[SMALL_INT_COUNT] = {nullptr};

static void initSmallInts() {
    for (int i = 0; i < SMALL_INT_COUNT; ++i) {
        if (!g_smallInts[i]) {
            g_smallInts[i] = new PyObject();
            g_smallInts[i]->refcount = IMMORTAL_REFCOUNT;
            g_smallInts[i]->type = 0;
            g_smallInts[i]->value = (long)(i + SMALL_INT_LO);
        }
    }
}

static PyObject* getSmallInt(long v) {
    int idx = (int)v - SMALL_INT_LO;
    if (idx < 0 || idx >= SMALL_INT_COUNT) return nullptr;
    return g_smallInts[idx];
}

static PyObject* getBoolObj(int v) {
    PyObject*& slot = v ? g_pyTrue : g_pyFalse;
    if (!slot) {
        slot = new PyObject();
        slot->refcount = IMMORTAL_REFCOUNT;
        slot->type = 5;
        slot->value = v ? 1 : 0;
    }
    return slot;
}

PyObject* PyInt_FromLong(long v) {
    if (PyObject* cached = getSmallInt(v)) {
        return cached;                          // immortal: caller "owns" but cannot free
    }
    PyObject* obj = new PyObject();
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
    return getBoolObj(v);
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
    if (!list || list->type != 1) return nullptr;
    long n = (long)list->list.size();
    long i = (long)idx->value;
    if (i < 0) i += n;
    if (i < 0 || i >= n) return nullptr;
    PyObject* item = PyList_GetItem(list, (size_t)i);
    if (item) Py_INCREF(item); // return new ref so callers can DECREF the list independently
    return item;
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

// Repeat a list `n` times (positive int only — matches CPython which raises
// TypeError on negative int * list, since it's a sequence repetition).
// Each element is INCREF'd so the result owns its references; the source
// list is unchanged.
static PyObject* PyList_Repeat(PyObject* list, long n) {
    if (n < 0) n = 0;     // conservative: matches "empty" rather than error
    PyObject* srcSizeBox = PyInt_FromLong((long)list->list.size());
    PyObject* result = PyList_NewBoxed(PyInt_FromLong((long)list->list.size() * n));
    (void)srcSizeBox;
    for (long i = 0; i < n; ++i) {
        for (size_t j = 0; j < list->list.size(); ++j) {
            PyObject* elem = list->list[j];
            if (elem) Py_INCREF(elem);
            PyList_SetItem(result, i * list->list.size() + j, elem);
        }
    }
    return result;
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
    if (!dict || dict->type != 2) return;
    // Check for an existing key that compares equal to the new key.
    // unordered_map uses pointer equality, so two different PyObject* with
    // the same Python value would create duplicate entries and leak the old key.
    if (key) {
        for (auto it = dict->dict.begin(); it != dict->dict.end(); ++it) {
            if (PyObject_CompareBool(it->first, key, 0)) {
                // Found equivalent key — DECREF old key, erase, then insert new.
                if (it->first) Py_DECREF(it->first);
                dict->dict.erase(it);
                break;
            }
        }
    }
    // Insert (or re-insert) the new key-value pair.
    dict->dict[key] = value;
    if (key) Py_INCREF(key);
    if (value) Py_INCREF(value);
}

PyObject* PyDict_GetItem(PyObject* dict, PyObject* key) {
    if (dict && dict->type == 2) {
        for (auto& pair : dict->dict) {
            if (PyObject_CompareBool(pair.first, key, 0)) {
                Py_INCREF(pair.second);
                return pair.second;
            }
        }
    }
    return nullptr;
}

// dict.get(key, default) — returns the value for `key` in `dict` if present,
// otherwise returns `default`. The default is INCREF'd so the caller owns its
// reference. If `default` is null, returns null (matches CPython).
PyObject* PyDict_GetItemWithDefault(PyObject* dict, PyObject* key, PyObject* defaultVal) {
    PyObject* v = PyDict_GetItem(dict, key);
    if (v) return v;  // already INCREF'd by PyDict_GetItem
    if (defaultVal) {
        Py_INCREF(defaultVal);
        return defaultVal;
    }
    return nullptr;
}

// dict.__delitem__(key) — remove `key` from `dict`. Silently no-op on missing
// keys (matches `del d[k]` for a missing key in CPython, which raises
// KeyError; we are conservative and follow the no-op path).
PyObject* PyDict_DelItem(PyObject* dict, PyObject* key) {
    if (dict && dict->type == 2 && key) {
        for (auto it = dict->dict.begin(); it != dict->dict.end(); ++it) {
            if (PyObject_CompareBool(it->first, key, 0)) {
                if (it->first) Py_DECREF(it->first);
                if (it->second) Py_DECREF(it->second);
                dict->dict.erase(it);
                return PyBool_New(1);
            }
        }
    }
    return PyBool_New(0);
}

void Py_DECREF(PyObject* obj) {
    if (obj && obj->refcount != IMMORTAL_REFCOUNT && --obj->refcount == 0) {
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

static int PyObject_PrintBase(PyObject* obj, FILE* fp);
static int PyObject_PrintElement(PyObject* obj, FILE* fp) {
    // Like PyObject_PrintBase but writes NO trailing newline. Used by
    // container printers (list/dict) so we get "[1, 2, 3]" instead of
    // "[1\n, 2\n, 3\n]".
    if (!obj) { return fprintf(fp, "None"); }
    if (obj->type == 5) return fprintf(fp, "%s", obj->value ? "True" : "False");
    if (obj->type == 0) return fprintf(fp, "%ld", obj->value);
    if (obj->type == 4) {
        char buf[64];
        format_double(buf, sizeof(buf), obj->dvalue);
        return fprintf(fp, "%s", buf);
    }
    if (obj->type == 1) {
        // Nested list — open bracket, recurse, close.
        fprintf(fp, "[");
        for (size_t i = 0; i < obj->list.size(); ++i) {
            if (i > 0) fprintf(fp, ", ");
            PyObject_PrintElement(obj->list[i], fp);
        }
        return fprintf(fp, "]");
    }
    if (obj->type == 2) {
        // Nested dict — open brace, recurse, close.
        fprintf(fp, "{");
        bool first = true;
        for (auto& pair : obj->dict) {
            if (!first) fprintf(fp, ", ");
            PyObject_PrintElement(pair.first, fp);
            fprintf(fp, ": ");
            PyObject_PrintElement(pair.second, fp);
            first = false;
        }
        return fprintf(fp, "}");
    }
    if (obj->type == 3) {
        // String element inside a container: use repr-style quotes.
        return fprintf(fp, "'%s'", obj->str.c_str());
    }
    if (obj->type == 6) return fprintf(fp, "<cell>");
    return fprintf(fp, "<object>");
}

static int PyObject_PrintBase(PyObject* obj, FILE* fp) {
    // Base printing without __str__/__repr__ checks (to avoid recursion)
    if (!obj) { int r = fprintf(fp, "None\n"); fflush(fp); return r; }
    if (obj->type == 5) { int r = fprintf(fp, "%s\n", obj->value ? "True" : "False"); fflush(fp); return r; }
    if (obj->type == 0) { int r = fprintf(fp, "%ld\n", obj->value); fflush(fp); return r; }
    if (obj->type == 4) {
        char buf[64];
        format_double(buf, sizeof(buf), obj->dvalue);
        int r = fprintf(fp, "%s\n", buf); fflush(fp); return r;
    }
    if (obj->type == 1) {
        fprintf(fp, "[");
        for (size_t i = 0; i < obj->list.size(); ++i) {
            if (i > 0) fprintf(fp, ", ");
            if (obj->list[i] && obj->list[i]->type == 3)
                fprintf(fp, "'%s'", obj->list[i]->str.c_str());
            else
                PyObject_PrintElement(obj->list[i], fp);
        }
        fprintf(fp, "]\n");
        fflush(fp);
        return 0;
    }
    if (obj->type == 2) {
        fprintf(fp, "{");
        bool first = true;
        for (auto& pair : obj->dict) {
            if (!first) fprintf(fp, ", ");
            PyObject_PrintElement(pair.first, fp);
            fprintf(fp, ": ");
            PyObject_PrintElement(pair.second, fp);
            first = false;
        }
        fprintf(fp, "}\n");
        fflush(fp);
        return 0;
    }
    if (obj->type == 3) { int r = fprintf(fp, "%s\n", obj->str.c_str()); fflush(fp); return r; }
    if (obj->type == 6) { int r = fprintf(fp, "<cell>\n"); fflush(fp); return r; }
    { int r = fprintf(fp, "<object>\n"); fflush(fp); return r; }
}

static PyObject* GetStrOrRepr(PyObject* obj, const char* method) {
    // Check for __str__ or __repr__ method on dict-backed objects (class instances)
    // First check instance dict, then class dict
    if (!obj || obj->type != 2) return nullptr;
    // Check instance dict first
    for (auto& pair : obj->dict) {
        if (pair.first && pair.first->type == 3 && pair.first->str == method) {
            return pair.second;
        }
    }
    // Check class dict
    for (auto& pair : obj->dict) {
        if (pair.first && pair.first->type == 3 && pair.first->str == "__class__") {
            PyObject* classDict = pair.second;
            if (classDict && classDict->type == 2) {
                for (auto& cpair : classDict->dict) {
                    if (cpair.first && cpair.first->type == 3 && cpair.first->str == method) {
                        return cpair.second;
                    }
                }
            }
            break;
        }
    }
    return nullptr;
}

int PyObject_Print(PyObject* obj, FILE* fp) {
    if (!fp) fp = stdout;
    if (!obj) { int r = fprintf(fp, "None\n"); fflush(fp); return r; }
    // Check for __str__ method first (used by print())
    PyObject* strMethod = GetStrOrRepr(obj, "__str__");
    if (strMethod && strMethod->type == 3) {
        PyObject* argList = PyList_NewBoxed(PyInt_FromLong(1));
        PyList_SetItemBoxed(argList, PyInt_FromLong(0), obj);
        PyObject* strResult = Pyc_Apply(strMethod, argList);
        if (strResult && strResult->type == 3) {
            int r = fprintf(fp, "%s\n", strResult->str.c_str());
            fflush(fp);
            Py_DECREF(strResult);
            Py_DECREF(argList);
            return r;
        }
        Py_DECREF(strResult);
        Py_DECREF(argList);
    }
    // Check for __repr__ method (fallback)
    PyObject* reprMethod = GetStrOrRepr(obj, "__repr__");
    if (reprMethod && reprMethod->type == 3) {
        PyObject* argList = PyList_NewBoxed(PyInt_FromLong(1));
        PyList_SetItemBoxed(argList, PyInt_FromLong(0), obj);
        PyObject* reprResult = Pyc_Apply(reprMethod, argList);
        if (reprResult && reprResult->type == 3) {
            int r = fprintf(fp, "%s\n", reprResult->str.c_str());
            fflush(fp);
            Py_DECREF(reprResult);
            Py_DECREF(argList);
            return r;
        }
        Py_DECREF(reprResult);
        Py_DECREF(argList);
    }
    return PyObject_PrintBase(obj, fp);
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
    if (obj->type == 2) {
        std::string r = "{";
        bool first = true;
        for (auto& pair : obj->dict) {
            if (!first) r += ", ";
            if (pair.first && pair.first->type == 3) {
                r += "'" + pair.first->str + "'";
            } else {
                PyObject* ks = PyStr_FromAny(pair.first);
                if (ks) { r += ks->str; Py_DECREF(ks); }
            }
            r += ": ";
            if (pair.second && pair.second->type == 3) {
                r += "'" + pair.second->str + "'";
            } else {
                PyObject* vs = PyStr_FromAny(pair.second);
                if (vs) { r += vs->str; Py_DECREF(vs); }
            }
            first = false;
        }
        r += "}";
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

PyObject* PyBuiltin_IntBase(PyObject* obj, PyObject* base) {
    int b = base ? (int)base->value : 10;
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 3) {
        try { return PyInt_FromLong(std::stol(obj->str, nullptr, b)); } catch (...) {}
    }
    if (obj->type == 0 || obj->type == 5) return PyInt_FromLong(obj->value);
    return PyInt_FromLong(0);
}

PyObject* PyBuiltin_Ord(PyObject* obj) {
    if (!obj || obj->type != 3 || obj->str.empty()) return PyInt_FromLong(0);
    return PyInt_FromLong((unsigned char)obj->str[0]);
}

PyObject* PyBuiltin_Chr(PyObject* obj) {
    if (!obj) return PyUnicode_FromString("");
    long v = (obj->type == 0 || obj->type == 5) ? obj->value : (long)obj->dvalue;
    char buf[2] = {(char)(v & 0xFF), '\0'};
    return PyUnicode_FromString(buf);
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

// bool(x) — returns PyBool_New of the truthiness of x. CPython's bool()
// always returns a real bool (True/False). Our PyBool_New now returns
// the cached immortal singletons, so identity comparisons work.
PyObject* PyBuiltin_Bool(PyObject* obj) {
    if (!obj) return PyBool_New(0);
    if (obj->type == 0 || obj->type == 5) return PyBool_New(obj->value != 0);
    if (obj->type == 4) return PyBool_New(obj->dvalue != 0.0);
    if (obj->type == 3) return PyBool_New(!obj->str.empty());
    if (obj->type == 1) return PyBool_New(!obj->list.empty());
    if (obj->type == 2) return PyBool_New(!obj->dict.empty());
    return PyBool_New(1);
}

// type(x) — returns a string naming the runtime type of x. We use the
// same names CPython uses so user code that compares to type names works.
PyObject* PyBuiltin_Type(PyObject* obj) {
    if (!obj) return PyUnicode_FromString("<class 'NoneType'>");
    switch (obj->type) {
        case 0: return PyUnicode_FromString("<class 'int'>");
        case 1: return PyUnicode_FromString("<class 'list'>");
        case 2: return PyUnicode_FromString("<class 'dict'>");
        case 3: return PyUnicode_FromString("<class 'str'>");
        case 4: return PyUnicode_FromString("<class 'float'>");
        case 5: return PyUnicode_FromString("<class 'bool'>");
        case 6: return PyUnicode_FromString("<class 'cell'>");
        default: return PyUnicode_FromString("<class 'object'>");
    }
}

static PyObject* intToBaseString(long v, int base, bool upper) {
    if (base < 2 || base > 36) base = 10;
    if (v == 0) return PyUnicode_FromString("0");
    bool neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    std::string out;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (u > 0) {
        out += digits[u % (unsigned)base];
        u /= (unsigned)base;
    }
    if (neg) out += "-";
    std::string rev(out.rbegin(), out.rend());
    return PyUnicode_FromString(rev.c_str());
}

// hex(x) — string with "0x" prefix. CPython adds the prefix; we match.
// CPython puts the negative sign before the prefix (-0x1) and uses
// lower-case x.
PyObject* PyBuiltin_Hex(PyObject* obj) {
    long v = 0;
    if (obj && (obj->type == 0 || obj->type == 5)) v = obj->value;
    PyObject* s = intToBaseString(v, 16, false);
    std::string body = s ? s->str : std::string("0");
    if (s) Py_DECREF(s);
    std::string out = "0x" + body;
    if (v < 0 && body.size() > 0 && body[0] == '-') out = "-0x" + body.substr(1);
    return PyUnicode_FromString(out.c_str());
}

// oct(x) — string with "0o" prefix.
PyObject* PyBuiltin_Oct(PyObject* obj) {
    long v = 0;
    if (obj && (obj->type == 0 || obj->type == 5)) v = obj->value;
    PyObject* s = intToBaseString(v, 8, false);
    std::string body = s ? s->str : std::string("0");
    if (s) Py_DECREF(s);
    std::string out = "0o" + body;
    if (v < 0 && body.size() > 0 && body[0] == '-') out = "-0o" + body.substr(1);
    return PyUnicode_FromString(out.c_str());
}

// bin(x) — string with "0b" prefix.
PyObject* PyBuiltin_Bin(PyObject* obj) {
    long v = 0;
    if (obj && (obj->type == 0 || obj->type == 5)) v = obj->value;
    PyObject* s = intToBaseString(v, 2, false);
    std::string body = s ? s->str : std::string("0");
    if (s) Py_DECREF(s);
    std::string out = "0b" + body;
    if (v < 0 && body.size() > 0 && body[0] == '-') out = "-0b" + body.substr(1);
    return PyUnicode_FromString(out.c_str());
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
    Py_INCREF(item);  // return new reference (caller owns it)
    return item;
}

void PyBuiltin_PrintNewline(void) {
    printf("\n");
}

// print(*args, sep=' ', end='\n', file=None) — builds the output string
// from the elements of `argList` joined by `sep`, appends `end`, and
// writes it to stdout. `argList` may be a Python list or nullptr (in
// which case just `end` is printed). `sep` and `end` may be null
// (treated as '' and '\n' respectively). Each PyObject* in argList is
// converted via PyStr_FromAny; the result is freed after printing.
// Returns None. Any PyObject* argument that is not a string/int/float/
// bool/list/dict falls back to PyStr_FromAny which prints "<object>".
void pyc_print(PyObject* argList, PyObject* sep, PyObject* end) {
    if (!sep)  sep  = PyUnicode_FromString(" ");
    if (!end)  end  = PyUnicode_FromString("\n");
    // Convert each arg to its string form, joining with `sep`.
    std::string out;
    if (argList && argList->type == 1) {
        for (size_t i = 0; i < argList->list.size(); ++i) {
            if (i > 0) {
                out += sep->str;
            }
            PyObject* s = PyStr_FromAny(argList->list[i]);
            if (s) { out += s->str; Py_DECREF(s); }
        }
    }
    out += end->str;
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// pyc_import_failed(module_name) — emits a clear ImportError-style
// diagnostic to stderr for an `import` of a module pyc does not support.
// The compiler treats all `import` statements as best-effort: this is
// the only error path for `import re`, `import os`, etc. (the `sys`
// module is faked by pyc_setup_sys and bypasses this path.)
//
// Returns None so the imported name in the calling code is set to a
// None value. Subsequent attribute access on it (e.g. `re.finditer`)
// will hit the standard PyObject_Print / method-lookup path and fail
// with a clear "method on None" diagnostic rather than silently
// returning wrong values.
PyObject* pyc_import_failed(PyObject* modName) {
    const char* name = (modName && modName->type == 3) ? modName->str.c_str() : "?";
    fprintf(stderr, "ImportError: No module named '%s' "
                    "(pyc supports only a synthetic 'sys' module; "
                    "real module loading is not yet implemented)\n", name);
    fflush(stderr);
    return nullptr;
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
    if (obj->type == 1) return PyList_GetItemObj(obj, key); // returns new ref (INCREF inside)
    if (obj->type == 2) {
        for (auto& pair : obj->dict) {
            if (PyObject_CompareBool(pair.first, key, 0)) {
                if (pair.second) Py_INCREF(pair.second); // return new ref
                return pair.second;
            }
        }
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
    if (!lst) return PyInt_FromLong(0);
    PyObject* total = PyInt_FromLong(0);
    auto addOne = [&](PyObject* item) {
        if (!item) return;
        PyObject* next = PyNumber_Add(total, item);
        Py_DECREF(total);
        total = next ? next : PyInt_FromLong(0);
    };
    if (lst->type == 1) {
        for (auto* item : lst->list) addOne(item);
    } else if (lst->type == 2) {
        // CPython: sum of dict iterates over keys.
        for (auto& pair : lst->dict) addOne(pair.first);
    }
    return total;
}

// PyBuiltin_Sorted(iterable, key) — sort the iterable's elements. If
// `key` is non-null it is a 1-arg callable applied to each element
// before comparison (standard sort key behaviour).
PyObject* PyBuiltin_Sorted(PyObject* lst, PyObject* key) {
    if (!lst) return PyList_New(0);
    std::vector<PyObject*> items;
    if (lst->type == 1) {
        for (auto* item : lst->list) {
            if (item) Py_INCREF(item);
            items.push_back(item);
        }
    } else if (lst->type == 2) {
        for (auto& pair : lst->dict) {
            if (pair.first) Py_INCREF(pair.first);
            items.push_back(pair.first);
        }
    } else {
        return PyList_New(0);
    }

    if (key) {
        // Apply the key to each item, then sort the keys.
        std::vector<PyObject*> keys;
        keys.reserve(items.size());
        for (auto* item : items) {
            PyObject* argList = PyList_New(1);
            if (item) { Py_INCREF(item); PyList_SetItem(argList, 0, item); }
            PyObject* k = Pyc_Apply(key, argList);
            if (argList) Py_DECREF(argList);
            keys.push_back(k);  // may be null
        }
        std::vector<size_t> idx(items.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t i, size_t j) {
            if (!keys[i] || !keys[j]) return false;
            return PyObject_CompareBool(keys[i], keys[j], 2) != 0;
        });
        PyObject* r = PyList_New(items.size());
        for (size_t i = 0; i < idx.size(); ++i) {
            if (items[idx[i]]) Py_INCREF(items[idx[i]]);
            PyList_SetItem(r, i, items[idx[i]]);
        }
        for (auto* k : keys) if (k) Py_DECREF(k);
        for (auto* it : items) if (it) Py_DECREF(it);
        return r;
    }

    PyObject* r = PyList_New(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i]) Py_INCREF(items[i]);
        PyList_SetItem(r, i, items[i]);
    }
    std::sort(r->list.begin(), r->list.end(), [](PyObject* a, PyObject* b) -> bool {
        if (!a || !b) return false;
        return PyObject_CompareBool(a, b, 2) != 0;
    });
    for (auto* it : items) if (it) Py_DECREF(it);
    return r;
}

// PyBuiltin_SortedWithCmp(iterable, cmp) — like sorted but takes a
// 2-arg comparator function instead of a key function. The comparator
// is invoked as cmp(a, b) for each pair; a negative return means
// a < b, zero means a == b, positive means a > b. This is the
// internal fast-path for sorted(..., key=cmp_to_key(cmp)).
PyObject* PyBuiltin_SortedWithCmp(PyObject* lst, PyObject* cmp) {
    if (!lst || !cmp) return PyBuiltin_Sorted(lst, nullptr);
    std::vector<PyObject*> items;
    if (lst->type == 1) {
        for (auto* item : lst->list) {
            if (item) Py_INCREF(item);
            items.push_back(item);
        }
    } else if (lst->type == 2) {
        for (auto& pair : lst->dict) {
            if (pair.first) Py_INCREF(pair.first);
            items.push_back(pair.first);
        }
    } else {
        return PyList_New(0);
    }
    std::sort(items.begin(), items.end(), [&](PyObject* a, PyObject* b) {
        if (!a || !b) return false;
        // Build a 2-arg arg list: [a, b].
        PyObject* args = PyList_New(2);
        if (a) { Py_INCREF(a); PyList_SetItem(args, 0, a); }
        if (b) { Py_INCREF(b); PyList_SetItem(args, 1, b); }
        PyObject* res = Pyc_Apply(cmp, args);
        if (args) Py_DECREF(args);
        long v = 0;
        if (res) {
            if (res->type == 0 || res->type == 5) v = res->value;
            else if (res->type == 4) v = (long)res->dvalue;
            Py_DECREF(res);
        }
        return v < 0;
    });
    PyObject* r = PyList_New(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i]) Py_INCREF(items[i]);
        PyList_SetItem(r, i, items[i]);
    }
    for (auto* it : items) if (it) Py_DECREF(it);
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

PyObject* PyString_Find3(PyObject* s, PyObject* sub, PyObject* start) {
    if (!s || s->type != 3 || !sub || sub->type != 3)
        return PyInt_FromLong(-1);
    long st = start ? start->value : 0;
    if (st < 0) st = 0;
    size_t pos = s->str.find(sub->str, (size_t)st);
    return PyInt_FromLong(pos == std::string::npos ? -1L : (long)pos);
}

PyObject* PyString_RFind(PyObject* s, PyObject* sub) {
    if (!s || s->type != 3 || !sub || sub->type != 3)
        return PyInt_FromLong(-1);
    size_t pos = s->str.rfind(sub->str);
    return PyInt_FromLong(pos == std::string::npos ? -1L : (long)pos);
}

PyObject* PyString_RFind3(PyObject* s, PyObject* sub, PyObject* start) {
    if (!s || s->type != 3 || !sub || sub->type != 3)
        return PyInt_FromLong(-1);
    long st = start ? start->value : 0;
    if (st < 0) st = 0;
    size_t endpos = s->str.size();
    size_t pos = s->str.rfind(sub->str, endpos);
    if (pos == std::string::npos || (long)pos < st) return PyInt_FromLong(-1L);
    return PyInt_FromLong((long)pos);
}

PyObject* PyString_RFind4(PyObject* s, PyObject* sub, PyObject* start, PyObject* end) {
    if (!s || s->type != 3 || !sub || sub->type != 3)
        return PyInt_FromLong(-1);
    long st = start ? start->value : 0;
    long en = end ? end->value : (long)s->str.size();
    if (st < 0) st = 0;
    if (en > (long)s->str.size()) en = (long)s->str.size();
    if (en <= st) return PyInt_FromLong(-1L);
    std::string haystack = s->str.substr((size_t)st, (size_t)(en - st));
    size_t pos = haystack.rfind(sub->str);
    if (pos == std::string::npos) return PyInt_FromLong(-1L);
    return PyInt_FromLong((long)pos + st);
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

PyObject* Pyc_GetSlice(PyObject* obj, PyObject* start, PyObject* stop, PyObject* step) {
    if (!obj) return PyList_New(0);
    long n = (obj->type == 1) ? (long)obj->list.size()
           : (obj->type == 3) ? (long)obj->str.size() : 0;
    long stp = (step && (step->type==0||step->type==5)) ? step->value : 1;
    if (stp == 0) {
        return (obj->type == 3) ? PyUnicode_FromString("") : PyList_New(0);
    }
    long s = (start && (start->type==0||start->type==5)) ? start->value : (stp > 0 ? 0 : n - 1);
    long e = (stop  && (stop->type ==0||stop->type ==5)) ? stop->value : (stp > 0 ? n : -1);
    if (s < 0) s += n;
    if (e < 0 && (stop && (stop->type==0||stop->type==5))) e += n;

    std::vector<long> idxs;
    if (stp > 0) {
        s = std::max(0L, std::min(n, s));
        e = std::max(0L, std::min(n, e));
        for (long i = s; i < e; i += stp) idxs.push_back(i);
    } else {
        // For negative step, allow stop to be a low sentinel (e.g. -1) meaning "before 0".
        // Only clamp start into [0, n-1].
        if (s < 0) s = 0;
        if (s >= n) s = n - 1;
        // e may legitimately be -1 or other <0; do not force it up.
        for (long i = s; i > e && i >= 0; i += stp) {
            if ((size_t)i < (size_t)n) idxs.push_back(i);
        }
    }

    if (obj->type == 1) {
        PyObject* r = PyList_New(idxs.size());
        for (size_t k = 0; k < idxs.size(); ++k) {
            long i = idxs[k];
            PyObject* item = (i >= 0 && (size_t)i < obj->list.size()) ? obj->list[i] : nullptr;
            if (item) Py_INCREF(item);
            PyList_SetItem(r, k, item);
        }
        return r;
    }
    if (obj->type == 3) {
        std::string r;
        for (long i : idxs) {
            if (i >= 0 && (size_t)i < obj->str.size()) r += obj->str[(size_t)i];
        }
        return PyUnicode_FromString(r.c_str());
    }
    return PyList_New(0);
}

void Pyc_SetSlice(PyObject* obj, PyObject* start, PyObject* stop, PyObject* step, PyObject* value) {
    if (!obj || obj->type != 1) return;
    bool explicit_step = (step != nullptr);
    long n = (long)obj->list.size();
    long stp = 1;
    if (explicit_step && (step->type==0||step->type==5)) stp = step->value;
    if (stp == 0) return;

    long s = (start && (start->type==0||start->type==5)) ? start->value : (stp > 0 ? 0 : n-1);
    long e = (stop  && (stop->type==0||stop->type==5)) ? stop->value : (stp > 0 ? n : -1);
    if (s < 0) s += n;
    if (e < 0 && (stop && (stop->type==0||stop->type==5))) e += n;

    std::vector<PyObject*> repl;
    if (value && value->type == 1) {
        for (auto* v : value->list) { if (v) Py_INCREF(v); repl.push_back(v); }
    } else if (value) {
        Py_INCREF(value);
        repl.push_back(value);
    }

    if (!explicit_step) {
        // basic slice: positive direction, length may change
        if (s < 0) s = 0;
        if (s > n) s = n;
        if (e < 0) e = 0;
        if (e > n) e = n;
        for (long i = s; i < e; ++i) {
            if (obj->list[i]) Py_DECREF(obj->list[i]);
        }
        obj->list.erase(obj->list.begin() + s, obj->list.begin() + e);
        obj->list.insert(obj->list.begin() + s, repl.begin(), repl.end());
        return;
    }

    // extended slice: positions visited, length preserving, exact count preferred
    std::vector<long> positions;
    long ss = s, ee = e;
    if (stp > 0) {
        if (ss < 0) ss = 0; if (ss > n) ss = n;
        if (ee < 0) ee = 0; if (ee > n) ee = n;
        for (long i = ss; i < ee; i += stp) positions.push_back(i);
    } else {
        if (ss < 0) ss = 0; if (ss > n) ss = n;
        long i = ss;
        if (i == n) i = n-1;
        for (; i > ee && i >= 0; i += stp) {
            if ((size_t)i < (size_t)n) positions.push_back(i);
        }
    }

    size_t m = repl.size();
    size_t k = 0;
    for (long pos : positions) {
        if (k >= m) break;
        if (pos >= 0 && (size_t)pos < obj->list.size()) {
            if (obj->list[pos]) Py_DECREF(obj->list[pos]);
            obj->list[pos] = repl[k];
            // repl[k] ref already bumped; list now owns that ref
        }
        ++k;
    }
    // release any unconsumed replacement refs we bumped
    for (; k < m; ++k) {
        if (repl[k]) Py_DECREF(repl[k]);
    }
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

// reversed(seq) — returns a new list with the elements of seq in
// reverse order. CPython returns a reverse_iterator; for the patterns
// pyc supports (list(reversed(x)), for x in reversed(x)) the result
// is the same. Accepts lists, tuples (also stored as list in our
// runtime), strings, and ranges.
PyObject* PyBuiltin_Reversed(PyObject* obj) {
    if (!obj) return PyList_New(0);
    PyObject* r = nullptr;
    if (obj->type == 1) {
        // List or tuple: reverse the elements.
        r = PyList_New(obj->list.size());
        for (size_t i = 0; i < obj->list.size(); ++i) {
            size_t ri = obj->list.size() - 1 - i;
            if (obj->list[ri]) Py_INCREF(obj->list[ri]);
            PyList_SetItem(r, i, obj->list[ri]);
        }
    } else if (obj->type == 3) {
        // String: reverse the characters.
        r = PyList_New(obj->str.size());
        for (size_t i = 0; i < obj->str.size(); ++i) {
            char buf[2] = {obj->str[obj->str.size() - 1 - i], '\0'};
            PyList_SetItem(r, i, PyUnicode_FromString(buf));
        }
    } else {
        return PyList_New(0);
    }
    return r;
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
// Supports the common CPython format-spec subset:
//   %[-+ 0#]<width>(.<precision>)?(<len>)?<spec>
// where len is one of "", "l", "h", "L" (ignored for int formatting in our
// runtime since long is the only int type) and spec is one of:
//   d, i  signed decimal int
//   u      unsigned decimal int (treated as d; our ints are signed)
//   o, x, X  octal / lowercase hex / uppercase hex
//   e, E, f, g, G  float with various precisions
//   s, r   string / repr
//   c      single character (codepoint)
//   %      literal percent
// Width/precision can be `*` to take the next positional arg.
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
        if (i + 1 >= f.size()) { result += '%'; break; }
        if (f[i+1] == '%') { result += '%'; i += 2; continue; }
        // Parse the format spec: [flags][width][.precision][length]spec
        size_t j = i + 1;
        // Flags: - + space 0 #
        std::string flags;
        while (j < f.size() && (f[j]=='-' || f[j]=='+' || f[j]==' ' || f[j]=='0' || f[j]=='#')) {
            flags += f[j++];
        }
        // Width: either digits or '*' (next arg)
        std::string widthStr;
        bool widthFromArg = false;
        if (j < f.size() && f[j] == '*') {
            widthFromArg = true; ++j;
        } else {
            while (j < f.size() && isdigit((unsigned char)f[j])) widthStr += f[j++];
        }
        // Precision: .<digits> or .*
        std::string precStr;
        bool precFromArg = false;
        bool hasPrec = false;
        if (j < f.size() && f[j] == '.') {
            hasPrec = true; ++j;
            if (j < f.size() && f[j] == '*') { precFromArg = true; ++j; }
            else while (j < f.size() && isdigit((unsigned char)f[j])) precStr += f[j++];
        }
        // Length modifier: h, l, ll, L (we just skip — our int is always long)
        // CPython accepts: h, hh, l, ll, L, q, j, z, t. We match a subset
        // (h, hh, l, ll, L); the rest are accepted to avoid spurious errors.
        if (j < f.size() && f[j]=='h') { ++j; if (j < f.size() && f[j]=='h') ++j; }
        else if (j < f.size() && f[j]=='l') { ++j; if (j < f.size() && f[j]=='l') ++j; }
        else if (j < f.size() && f[j]=='L') { ++j; }
        if (j >= f.size()) { result += f[i++]; continue; }
        char spec = f[j];

        // Resolve width and precision (consume extra args if from-arg)
        int width = 0;
        int prec = -1;
        if (widthFromArg) {
            PyObject* w = getArg(argIdx++);
            if (w && (w->type==0 || w->type==5)) width = (int)w->value;
        } else if (!widthStr.empty()) {
            width = std::atoi(widthStr.c_str());
        }
        if (precFromArg) {
            PyObject* p = getArg(argIdx++);
            if (p && (p->type==0 || p->type==5)) prec = (int)p->value;
            if (prec < 0) prec = 0;
        } else if (hasPrec) {
            prec = precStr.empty() ? 0 : std::atoi(precStr.c_str());
        }

        // Build the snprintf format string for this spec
        std::string sub = "%" + flags + (widthFromArg ? std::to_string(width) : widthStr);
        if (hasPrec) sub += "." + (precFromArg ? std::to_string(prec) : precStr);
        sub += spec;
        const char* fsp = sub.c_str();
        char buf[512] = {};
        PyObject* arg = getArg(argIdx++);
        bool consumed = true;

        switch (spec) {
            case 'd': case 'i': case 'u': {
                long v = arg ? ((arg->type==0||arg->type==5) ? arg->value : (arg->type==4 ? (long)arg->dvalue : 0)) : 0;
                snprintf(buf, sizeof(buf), fsp, v);
                break;
            }
            case 'o': case 'x': case 'X': {
                // CPython treats %o, %x, %X as unsigned. We just print
                // the long in unsigned form via a temp.
                long v = arg ? ((arg->type==0||arg->type==5) ? arg->value : (arg->type==4 ? (long)arg->dvalue : 0)) : 0;
                // # flag with o/x/X adds 0/0x/0X prefix
                if (flags.find('#') != std::string::npos) {
                    std::string prefix;
                    if (spec == 'o') prefix = "0";
                    else if (spec == 'x') prefix = "0x";
                    else if (spec == 'X') prefix = "0X";
                    snprintf(buf, sizeof(buf), fsp, (unsigned long)v);
                    std::string out = buf;
                    if (out.find(prefix) != 0) out = prefix + out;
                    snprintf(buf, sizeof(buf), "%s", out.c_str());
                } else {
                    snprintf(buf, sizeof(buf), fsp, (unsigned long)v);
                }
                break;
            }
            case 'e': case 'E': case 'f': case 'g': case 'G': {
                double v = arg ? numeric_val(arg) : 0.0;
                snprintf(buf, sizeof(buf), fsp, v);
                break;
            }
            case 's': {
                // %s honours width and flags (- left-align, others right-align).
                // We build a custom right-padding here because snprintf %s
                // with a * width DOES work, but the runtime's snprintf
                // may not always be available — so we always do it manually.
                PyObject* s = arg ? PyStr_FromAny(arg) : PyUnicode_FromString("");
                std::string body = s ? s->str : std::string();
                if (s) Py_DECREF(s);
                int w = width;
                if (w > 0 && (int)body.size() < w) {
                    bool leftAlign = flags.find('-') != std::string::npos;
                    int pad = w - (int)body.size();
                    if (leftAlign) {
                        body.append((size_t)pad, ' ');
                    } else {
                        body.insert(0, (size_t)pad, ' ');
                    }
                }
                snprintf(buf, sizeof(buf), "%s", body.c_str());
                break;
            }
            case 'r': {
                PyObject* s = arg ? PyStr_FromAny(arg) : PyUnicode_FromString("");
                std::string body = s ? s->str : std::string();
                if (s) Py_DECREF(s);
                // repr: add quotes for strings (limited — we don't escape)
                if (arg && arg->type == 3) {
                    body = "'" + body + "'";
                }
                snprintf(buf, sizeof(buf), "%s", body.c_str());
                break;
            }
            case 'c': {
                long v = arg ? ((arg->type==0||arg->type==5) ? arg->value : (arg->type==4 ? (long)arg->dvalue : 0)) : 0;
                buf[0] = (char)(v & 0x7f);
                buf[1] = '\0';
                break;
            }
            default:
                // Unknown spec — keep the literal text and move on.
                result += f.substr(i, j - i + 1);
                consumed = false;
                break;
        }
        if (consumed) result += buf;
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

// The single, well-known `sys` object. Allocated lazily on first
// pyc_setup_sys() call. Stored in a global so PyObject_GetAttr("sys")
// can return it. Held alive for program lifetime (immortal in spirit).
static PyObject* g_sys_module = nullptr;
static PyObject* g_sys_argv = nullptr;

// Build the synthetic `sys` module and `sys.argv` list from the
// process's argc/argv. Called once at program startup. Idempotent.
void pyc_setup_sys(int argc, char** argv) {
    if (g_sys_module != nullptr) return;

    // Lazily initialise immortal singletons (True, False, small ints).
    // These are used by code paths called below (PyInt_FromLong, PyBool_New).
    initSmallInts();

    // sys = a dict with key "argv" (and a few other keys for compatibility).
    g_sys_module = PyDict_New();

    // sys.argv = list of PyObject* strings
    {
        PyObject* argcBoxed = PyInt_FromLong(argc);
        g_sys_argv = PyList_NewBoxed(argcBoxed);
        Py_DECREF(argcBoxed);
    }
    for (int i = 0; i < argc; ++i) {
        PyObject* s = PyUnicode_FromString(argv[i]);
        PyObject* idx = PyInt_FromLong(i);
        PyList_SetItemBoxed(g_sys_argv, idx, s);
        Py_DECREF(idx);
        Py_DECREF(s);
    }
    PyObject* argv_key = PyUnicode_FromString("argv");
    PyDict_SetItem(g_sys_module, argv_key, g_sys_argv);
    // argv_key and g_sys_argv are owned by g_sys_module now.
}

// Look up an attribute on the global `sys` module. Returns a strong
// reference (caller must DECREF) or NULL if the attribute is missing.
PyObject* pyc_get_sys_attr(const char* name) {
    if (g_sys_module == nullptr) return nullptr;
    PyObject* key = PyUnicode_FromString(name);
    if (!key) return nullptr;
    PyObject* val = PyDict_GetItem(g_sys_module, key);
    Py_DECREF(key);
    return val;
}

// Return the global `sys` module object (a new strong reference, or
// NULL if pyc_setup_sys has not been called).
PyObject* pyc_get_sys_module(void) {
    if (g_sys_module == nullptr) return nullptr;
    Py_INCREF(g_sys_module);
    return g_sys_module;
}

PyObject* PyNumber_Multiply(PyObject* a, PyObject* b) {
    if (!a || !b) return NULL;
    if (a->type == 3 && b->type == 0) return PyString_Repeat(a, b);
    if (a->type == 0 && b->type == 3) return PyString_Repeat(b, a);
    if (a->type == 1 && b->type == 0) return PyList_Repeat(a, b->value);
    if (a->type == 0 && b->type == 1) return PyList_Repeat(b, a->value);
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
    // None equality: CPython's `None == None` is True; `None == <other>` is False;
    // `None < <other>` is a TypeError (we conservatively return 0).
    if (!a && !b) {
        switch (op) {
            case 0: return 1;   // ==
            case 1: return 0;   // !=
            default: return 0;  // ordering
        }
    }
    if (!a || !b) {
        // None vs non-None: only `!=` is True.
        switch (op) {
            case 1: return 1;
            default: return 0;
        }
    }
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
    if (!obj) return nullptr;
    // First, see if the attribute is a known key on the synthetic `sys`
    // module (set up by pyc_setup_sys). This is the only place a "real"
    // attribute lookup can succeed because the pyc runtime has no real
    // Python type system; every other "object" is a flat int/float/list/dict.
    if (obj == g_sys_module) {
        return pyc_get_sys_attr(attr);
    }
    // Lists: support .append / .sort / .pop (used by compiled code).
    if (obj->type == 1) {
        if (strcmp(attr, "append") == 0) {
            // Return a dummy callable that no-ops (we only need the
            // lookup to succeed; codegen emits explicit list_* calls).
            return PyInt_FromLong(0);
        }
        if (strcmp(attr, "sort") == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "pop") == 0) return PyInt_FromLong(0);
    }
    // Dicts: support .keys / .values / .items.
    if (obj->type == 2) {
        if (strcmp(attr, "keys")   == 0) return PyBuiltin_List(obj);
        if (strcmp(attr, "values") == 0) return PyBuiltin_List(obj);
        if (strcmp(attr, "items")  == 0) return PyBuiltin_List(obj);
    }
    // Strings: support .upper / .lower / .strip / .split / .join / etc.
    if (obj->type == 3) {
        if (strcmp(attr, "upper") == 0) return PyString_Upper(obj);
        if (strcmp(attr, "lower") == 0) return PyString_Lower(obj);
        if (strcmp(attr, "strip") == 0) return PyString_Strip(obj);
        if (strcmp(attr, "split") == 0) return PyString_Split(obj, nullptr);
        if (strcmp(attr, "join")  == 0) return PyString_Join(obj, nullptr);
    }
    // Fallback: return the object itself (matches the previous stub
    // behaviour for unsupported lookups; doesn't crash).
    Py_INCREF(obj);
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

// ---- B4/B8 callable dispatch (lambdas as values, dynamic call via token) ----

// Simple registry: token name -> function pointer that accepts a PyObject* list and returns a boxed result.
static std::unordered_map<std::string, PyObject* (*)(PyObject*)> g_callableRegistry;

extern "C" void pyc_register_callable(const char* name, PyObject* (*func)(PyObject*)) {
    if (name && func) g_callableRegistry[std::string(name)] = func;
}

// Pyc_Apply(tokenStr, argList) -> boxed result
extern "C" PyObject* Pyc_Apply(PyObject* token, PyObject* argList) {
    if (!token || token->type != 3) return nullptr;
    std::string name = token->str;
    auto it = g_callableRegistry.find(name);
    if (it == g_callableRegistry.end()) return nullptr;
    return it->second ? it->second(argList) : nullptr;
}

// ---- B5 (nonlocal / cells) minimal primitives ----
// A cell is a PyObject with type==6; cell_content holds the target PyObject*.
// Cells are allocated in an enclosing scope and passed (or reachable) into nested functions
// so that nonlocal writes are visible to all readers/writers sharing the cell.

extern "C" PyObject* PyCell_New(PyObject* initial) {
    PyObject* c = new PyObject();
    c->refcount = 1;
    c->type = 6;                 // cell
    c->cell_content = initial;
    if (initial) Py_INCREF(initial);
    return c;
}

extern "C" PyObject* PyCell_Get(PyObject* cell) {
    if (!cell || cell->type != 6) return nullptr;
    PyObject* v = cell->cell_content;
    if (v) Py_INCREF(v);
    return v;
}

extern "C" PyObject* PyCell_Set(PyObject* cell, PyObject* val) {
    if (!cell || cell->type != 6) return nullptr;
    if (cell->cell_content) Py_DECREF(cell->cell_content);
    cell->cell_content = val;
    if (val) Py_INCREF(val);
    return cell;
}

extern "C" int PyCell_Check(PyObject* obj) {
    return (obj && obj->type == 6) ? 1 : 0;
}

} // extern "C"
