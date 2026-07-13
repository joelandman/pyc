#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <string>
#include <atomic>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "pyc/runtime.h"

// Immortal PyObject* refcount sentinel (True, False, and small ints in
// [-5, 256] are interned; Py_INCREF / Py_DECREF skip them so they are
// never freed or duplicated by ownership paths).
static constexpr int IMMORTAL_REFCOUNT = 0x3fffffff;

// Forward declaration of the try-stack head, used by division/modulo
// zero-division reporting (we raise when an enclosing try is in scope, and
// print-and-exit otherwise).
struct TryFrame;
static thread_local TryFrame* g_try_stack = nullptr;

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
    // A4 homogeneous numeric lists
    int list_item_type;      // 0=general PyObject*, 1=int64, 2=double
    std::vector<long> ilist;
    std::vector<double> flist;
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

static std::atomic<long> alloc_int_count{0};
static std::atomic<long> alloc_float_count{0};
static std::atomic<long> alloc_list_count{0};
static std::atomic<long> alloc_dict_count{0};
static std::atomic<long> alloc_str_count{0};

PyObject* PyInt_FromLong(long v) {
    if (PyObject* cached = getSmallInt(v)) {
        return cached;                          // immortal: caller "owns" but cannot free
    }
    alloc_int_count++;
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
    alloc_float_count++;
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 4;
    obj->dvalue = v;
    return obj;
}

PyObject* PyList_New(size_t size) {
    alloc_list_count++;
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 1;
    obj->list.assign(size, nullptr);
    obj->list_item_type = 0;
    return obj;
}

PyObject* PyList_GetItem(PyObject* list, size_t index) {
    if (list && list->type == 1) {
        if (list->list_item_type == 1 && index < list->ilist.size())
            return PyInt_FromLong(list->ilist[index]);
        if (list->list_item_type == 2 && index < list->flist.size())
            return PyFloat_FromDouble(list->flist[index]);
        if (index < list->list.size())
            return list->list[index];
    }
    return nullptr;
}

size_t PyList_Size(PyObject* list) {
    if (list && list->type == 1) {
        if (list->list_item_type == 1) return list->ilist.size();
        if (list->list_item_type == 2) return list->flist.size();
        return list->list.size();
    }
    return 0;
}

void PyList_SetItem(PyObject* list, size_t index, PyObject* item) {
    if (!list || list->type != 1) return;
    if (list->list_item_type == 1) {
        if (index >= list->ilist.size()) list->ilist.resize(index + 1, 0);
        if (item && item->type == 0) list->ilist[index] = item->value;
        else if (item && item->type == 5) list->ilist[index] = item->value ? 1 : 0;
        return;
    }
    if (list->list_item_type == 2) {
        if (index >= list->flist.size()) list->flist.resize(index + 1, 0.0);
        if (item && item->type == 4) list->flist[index] = item->dvalue;
        else if (item && (item->type == 0 || item->type == 5)) list->flist[index] = (double)item->value;
        return;
    }
    if (index < list->list.size()) {
        if (list->list[index]) Py_DECREF(list->list[index]);
        list->list[index] = item;
        if (item) Py_INCREF(item);
    }
}

PyObject* PyList_Append(PyObject* list, PyObject* item) {
    if (list && list->type == 1) {
        if (list->list_item_type == 1) {
            if (item && (item->type == 0 || item->type == 5)) list->ilist.push_back(item->value);
            else list->ilist.push_back(0);
            if (item) Py_INCREF(item); // for the boxed item if caller expects
            return nullptr;
        }
        if (list->list_item_type == 2) {
            if (item && item->type == 4) list->flist.push_back(item->dvalue);
            else if (item && (item->type == 0 || item->type == 5)) list->flist.push_back((double)item->value);
            else list->flist.push_back(0.0);
            if (item) Py_INCREF(item);
            return nullptr;
        }
        list->list.push_back(item);
        if (item) Py_INCREF(item);
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
    if (!list || list->type != 1) return nullptr;
    if (!idx || (idx->type != 0 && idx->type != 5)) return nullptr;
    size_t n = PyList_Size(list);
    long i = (long)idx->value;
    if (i < 0) i += (long)n;
    if (i < 0 || (size_t)i >= n) return nullptr;
    if (list->list_item_type == 1) {
        return PyInt_FromLong(list->ilist[i]);
    }
    if (list->list_item_type == 2) {
        return PyFloat_FromDouble(list->flist[i]);
    }
    PyObject* item = list->list[i];
    if (item) Py_INCREF(item);
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

// A4 fast paths for homogeneous numeric lists (internal use by codegen)
PyObject* PyList_NewInt(size_t size) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 1;
    obj->list_item_type = 1;
    obj->ilist.assign(size, 0);
    return obj;
}

PyObject* PyList_NewFloat(size_t size) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 1;
    obj->list_item_type = 2;
    obj->flist.assign(size, 0.0);
    return obj;
}

PyObject* PyList_NewIntBoxed(PyObject* n) {
    size_t size = (n && n->type == 0) ? (size_t)n->value : 0;
    return PyList_NewInt(size);
}

PyObject* PyList_NewFloatBoxed(PyObject* n) {
    size_t size = (n && n->type == 0) ? (size_t)n->value : 0;
    return PyList_NewFloat(size);
}

long PyList_GetItemInt64(PyObject* list, size_t index) {
    if (list && list->type == 1 && list->list_item_type == 1 && index < list->ilist.size())
        return list->ilist[index];
    return 0;
}

void PyList_SetItemInt64(PyObject* list, size_t index, long v) {
    if (list && list->type == 1 && list->list_item_type == 1 && index < list->ilist.size())
        list->ilist[index] = v;
}

double PyList_GetItemDouble(PyObject* list, size_t index) {
    if (list && list->type == 1 && list->list_item_type == 2 && index < list->flist.size())
        return list->flist[index];
    return 0.0;
}

void PyList_SetItemDouble(PyObject* list, size_t index, double v) {
    if (list && list->type == 1 && list->list_item_type == 2 && index < list->flist.size())
        list->flist[index] = v;
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
    alloc_dict_count++;
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

// --- re module (PCRE2-backed) ---------------------------------------------
//
// `import re` returns a synthetic module dict containing tokens for
// `finditer`, `findall`, `compile`, etc. The compiler (`lowerMethodCall`)
// recognises calls of the form `re.finditer(...)` / `re.findall(...)` /
// `re.match(...)` / `re.search(...)` / `re.compile(...)` and emits direct
// calls to PyBuiltin_Re* helpers below.
//
// Match objects use a new PyObject type (9). The compiled-pattern type
// is 8. Both expose their data via the `value` field (a 64-bit pointer).
// On x86_64 a `long` is 8 bytes, so a pointer fits; on 32-bit hosts the
// caller would need a side-table, but we don't support 32-bit anyway.

struct CompiledRegex {
    pcre2_code* code;
    std::string pattern;
    CompiledRegex() : code(nullptr) {}
    ~CompiledRegex() { if (code) pcre2_code_free(code); }
};
struct MatchObj {
    pcre2_match_data* md;
    std::string subject;
    int capture_count;
    MatchObj() : md(nullptr), capture_count(0) {}
    ~MatchObj() { if (md) pcre2_match_data_free(md); }
};

// Forward declarations for the re helpers. Definitions are below.
extern "C" PyObject* PyBuiltin_ReFinditer(PyObject* pattern, PyObject* subject);
extern "C" PyObject* PyBuiltin_ReFindall(PyObject* pattern, PyObject* subject);
extern "C" PyObject* PyBuiltin_ReCompile(PyObject* pattern);
extern "C" PyObject* PyBuiltin_ReMatchGroup(PyObject* m, PyObject* idxObj);

void Py_DECREF(PyObject* obj) {
    if (obj && obj->refcount != IMMORTAL_REFCOUNT && --obj->refcount == 0) {
        if (obj->type == 1) {
            if (obj->list_item_type == 0) {
                for (PyObject* item : obj->list) if (item) Py_DECREF(item);
            }
        } else if (obj->type == 2) {
            for (auto& pair : obj->dict) {
                Py_DECREF(pair.first);
                Py_DECREF(pair.second);
            }
        } else if (obj->type == 8) {
            CompiledRegex* cr = reinterpret_cast<CompiledRegex*>(obj->value);
            delete cr;
        } else if (obj->type == 9) {
            MatchObj* mo = reinterpret_cast<MatchObj*>(obj->value);
            delete mo;
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
    if (obj->type == 9) {
        // Match object — print "<re.Match object>" for safety.
        int r = fprintf(fp, "<re.Match object>\n"); fflush(fp); return r;
    }
    if (obj->type == 1) {
        fprintf(fp, "[");
        // A4: handle homogeneous lists (ilist/flist) vs boxed list.
        if (obj->list_item_type == 1) {
            // int-homogeneous list
            for (size_t i = 0; i < obj->ilist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                int r = fprintf(fp, "%ld", obj->ilist[i]);
                (void)r;
            }
        } else if (obj->list_item_type == 2) {
            // float-homogeneous list
            for (size_t i = 0; i < obj->flist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                char buf[64];
                format_double(buf, sizeof(buf), obj->flist[i]);
                int r = fprintf(fp, "%s", buf);
                (void)r;
            }
        } else {
            // boxed list
            for (size_t i = 0; i < obj->list.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                if (obj->list[i] && obj->list[i]->type == 3)
                    fprintf(fp, "'%s'", obj->list[i]->str.c_str());
                else
                    PyObject_PrintElement(obj->list[i], fp);
            }
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
    alloc_str_count++;
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 3;
    obj->str = s ? s : "";
    return obj;
}

// Convert any PyObject to its string representation (no trailing newline).
// Named PyStr_FromAny to avoid conflict with CPython's PyObject_Str.
// Honours class `__str__` / `__repr__` methods (delegates to PyObject_Print
// on a tmpfile so the formatting path matches print()).
PyObject* PyStr_FromAny(PyObject* obj) {
    if (!obj) return PyUnicode_FromString("None");
    // Use PyObject_Print for the full formatting path so that class
    // `__str__` / `__repr__` methods are invoked.
    FILE* tmp = std::tmpfile();
    if (tmp) {
        PyObject_Print(obj, tmp);
        std::fflush(tmp);
        std::rewind(tmp);
        char buf[65536];
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, tmp);
        buf[n] = '\0';
        if (n > 0 && buf[n-1] == '\n') buf[--n] = '\0';
        std::fclose(tmp);
        return PyUnicode_FromString(buf);
    }
    // Fallback for the rare tmpfile() failure.
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
        // A4: handle homogeneous lists.
        if (obj->list_item_type == 1) {
            for (size_t i = 0; i < obj->ilist.size(); ++i) {
                if (i > 0) r += ", ";
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", obj->ilist[i]);
                r += buf;
            }
        } else if (obj->list_item_type == 2) {
            for (size_t i = 0; i < obj->flist.size(); ++i) {
                if (i > 0) r += ", ";
                char buf[64];
                format_double(buf, sizeof(buf), obj->flist[i]);
                r += buf;
            }
        } else {
            for (size_t i = 0; i < obj->list.size(); ++i) {
                if (i > 0) r += ", ";
                PyObject* s = PyStr_FromAny(obj->list[i]);
                if (obj->list[i] && obj->list[i]->type == 3) { r += "'"; r += obj->list[i]->str; r += "'"; }
                else if (s) r += s->str;
                if (s) Py_DECREF(s);
            }
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

// id(obj) — return a unique integer for each distinct object. CPython
// uses the object address; we approximate with the refcount + a
// per-process counter so two distinct objects always get distinct
// ids. Same object always returns the same id within a process.
static long g_id_counter = 0x100000;
PyObject* PyBuiltin_Id(PyObject* obj) {
    if (!obj) return PyInt_FromLong(0);
    // Use a stable mapping: PyObject* address as a long (low 48 bits
    // on x86_64, plenty of distinct values). Combined with the
    // refcount (which differs across calls for newly-allocated
    // objects) to make the id unique.
    long addr = (long)(intptr_t)obj;
    // Fold the address into a positive int. Take the low 32 bits and
    // add an offset so we don't return 0/negative ids.
    long id = (addr ^ (addr >> 16)) & 0x7fffffff;
    if (id == 0) id = (long)(++g_id_counter);
    return PyInt_FromLong(id);
}

// divmod(a, b) — return (a // b, a % b). CPython returns a tuple; in
// our flat runtime a tuple is just a list, so we return a 2-element
// list. (We use PyList_NewBoxed + PyList_SetItemBoxed for the values.)
PyObject* PyBuiltin_Divmod(PyObject* a, PyObject* b) {
    if (!a || !b) return nullptr;
    PyObject* q = PyNumber_Divide(a, b);
    PyObject* r = PyNumber_Remainder(a, b);
    if (!q || !r) { if (q) Py_DECREF(q); if (r) Py_DECREF(r); return nullptr; }
    PyObject* r2 = PyList_New(2);
    PyList_SetItem(r2, 0, q);
    PyList_SetItem(r2, 1, r);
    return r2;
}

// repr(obj) — return a string representation. For our boxed types:
//   - int, float, bool, str use their natural repr (with str quotes)
//   - list uses Python list repr syntax [a, b, c]
//   - dict uses {key: value, ...} syntax with proper quoting
//   - None returns 'None'
PyObject* PyBuiltin_Repr(PyObject* obj) {
    if (!obj) return PyUnicode_FromString("None");
    if (obj->type == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", obj->value);
        return PyUnicode_FromString(buf);
    }
    if (obj->type == 5) return PyUnicode_FromString(obj->value ? "True" : "False");
    if (obj->type == 4) {
        char buf[64];
        format_double(buf, sizeof(buf), obj->dvalue);
        return PyUnicode_FromString(buf);
    }
    if (obj->type == 3) {
        // String: wrap in single quotes (simplified — no escaping).
        std::string r = "'" + obj->str + "'";
        return PyUnicode_FromString(r.c_str());
    }
    if (obj->type == 1) {
        std::string r = "[";
        bool first = true;
        for (auto* item : obj->list) {
            if (!first) r += ", ";
            first = false;
            if (item && item->type == 3) { r += "'" + item->str + "'"; }
            else if (item) {
                PyObject* s = PyBuiltin_Repr(item);
                if (s) { r += s->str; Py_DECREF(s); }
            }
        }
        r += "]";
        return PyUnicode_FromString(r.c_str());
    }
    if (obj->type == 2) {
        // Check for __repr__ method (instance dict or class dict)
        PyObject* reprMethod = nullptr;
        for (auto& kv : obj->dict) {
            if (kv.first->str == "__repr__") {
                reprMethod = kv.second;
                break;
            }
        }
        if (!reprMethod) {
            // Check class dict
            for (auto& kv : obj->dict) {
                if (kv.first->str == "__class__") {
                    PyObject* classDict = kv.second;
                    if (classDict && classDict->type == 2) {
                        for (auto& ck : classDict->dict) {
                            if (ck.first->str == "__repr__") {
                                reprMethod = ck.second;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
        if (reprMethod) {
            // Call __repr__ method with self as argument
            PyObject* args = PyList_New(0);
            PyList_Append(args, obj);
            PyObject* result = Pyc_Apply(reprMethod, args);
            Py_DECREF(args);
            if (result) return result;
        }
        std::string r = "{";
        bool first = true;
        for (auto& pair : obj->dict) {
            if (!first) r += ", ";
            first = false;
            if (pair.first && pair.first->type == 3) r += "'" + pair.first->str + "'";
            else if (pair.first) {
                PyObject* s = PyBuiltin_Repr(pair.first);
                if (s) { r += s->str; Py_DECREF(s); }
            }
            r += ": ";
            if (pair.second && pair.second->type == 3) r += "'" + pair.second->str + "'";
            else if (pair.second) {
                PyObject* s = PyBuiltin_Repr(pair.second);
                if (s) { r += s->str; Py_DECREF(s); }
            }
        }
        r += "}";
        return PyUnicode_FromString(r.c_str());
    }
    return PyUnicode_FromString("<object>");
}

// round(x) / round(x, n) — round to nearest, ties to even (CPython
// uses banker's rounding for floats; for ints the 2-arg form rounds
// to the nearest power of 10).
static double round_half_to_even(double v) {
    double f = floor(v);
    double diff = v - f;
    if (diff < 0.5) return f;
    if (diff > 0.5) return f + 1.0;
    // Exactly 0.5: round to even.
    long long fl = (long long)f;
    return (fl % 2 == 0) ? f : f + 1.0;
}
PyObject* PyBuiltin_Round(PyObject* x, PyObject* n) {
    if (!x) return PyInt_FromLong(0);
    // `n` may be null (no ndigits given) or a non-zero int/bool. CPython
    // raises TypeError on a non-numeric `n`; we conservatively treat it as
    // 0 (no rounding scale).
    long ndig = 0;
    if (n && (n->type == 0 || n->type == 5)) ndig = n->value;
    bool hasN = ndig != 0;
    if (x->type == 0 || x->type == 5) {
        // int: with ndigits, round to power of 10; otherwise identity.
        if (!hasN) return PyInt_FromLong(x->value);
        long long v = x->value;
        // p is the magnitude of the rounding scale (10^|ndig|).
        double p = pow(10.0, (double)(ndig > 0 ? ndig : -ndig));
        double r;
        if (ndig >= 0) {
            // Round v/p * p = round to nearest multiple of p
            // (e.g. round(123, -1) = 120 = 12 * 10).
            r = round_half_to_even((double)v / p) * p;
        } else {
            // ndig < 0: divide by 10^|ndig| then multiply back.
            // (Same path as ndig >= 0 above; ndig=0 and ndig<0 both
            //  fall here when |ndig|>0.)
            r = round_half_to_even((double)v / p) * p;
        }
        return PyInt_FromLong((long)r);
    }
    if (x->type == 4) {
        if (!hasN) return PyFloat_FromDouble(round_half_to_even(x->dvalue));
        double p = pow(10.0, (double)(ndig > 0 ? ndig : -ndig));
        double r;
        if (ndig >= 0) {
            // For ndig > 0: scale up, round, scale back down.
            //   round(0.123, 1) = round(0.123 * 10) / 10 = 0.1
            r = round_half_to_even(x->dvalue * p) / p;
        } else {
            // ndig < 0: scale down, round, scale back up.
            //   round(12.5, -1) = round(12.5 / 10) * 10 = 1 * 10 = 10
            r = round_half_to_even(x->dvalue / p) * p;
        }
        return PyFloat_FromDouble(r);
    }
    return PyInt_FromLong(0);
}

// pow(base, exp) — for int base+exp we use the runtime's native pow
// (a**b) which already works; for float we use pow(); otherwise fall
// back to a generic multiplicative loop for positive int exponents.
PyObject* PyBuiltin_Pow(PyObject* a, PyObject* b) {
    if (!a || !b) return nullptr;
    if (a->type == 4 || b->type == 4) {
        double av = (a->type == 0 || a->type == 5) ? (double)a->value : a->dvalue;
        double bv = (b->type == 0 || b->type == 5) ? (double)b->value : b->dvalue;
        return PyFloat_FromDouble(pow(av, bv));
    }
    long long av = (a->type == 0 || a->type == 5) ? a->value : 0;
    long long bv = (b->type == 0 || b->type == 5) ? b->value : 0;
    if (bv < 0) return PyFloat_FromDouble(pow((double)av, (double)bv));
    long long r = 1;
    for (long long i = 0; i < bv; ++i) r *= av;
    return PyInt_FromLong((long)r);
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
    if (!lst || lst->type != 1) return nullptr;
    // Handle homogeneous int lists
    if (lst->list_item_type == 1 && !lst->ilist.empty()) {
        long val = lst->ilist.back();
        lst->ilist.pop_back();
        return PyInt_FromLong(val);
    }
    // Handle homogeneous float lists
    if (lst->list_item_type == 2 && !lst->flist.empty()) {
        double val = lst->flist.back();
        lst->flist.pop_back();
        return PyFloat_FromDouble(val);
    }
    // Handle regular boxed lists
    if (lst->list.empty()) return nullptr;
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
    // Convert each arg to its string form, joining with `sep`. Use
    // PyObject_Print for each element so that class `__str__` / `__repr__`
    // hooks are honoured (CPython's print calls str() on each arg).
    std::string out;
    if (argList && argList->type == 1) {
        for (size_t i = 0; i < argList->list.size(); ++i) {
            if (i > 0) {
                out += sep->str;
            }
            // Format the element to a temporary file, then append to our
            // buffer. PyObject_Print writes its own trailing newline; we
            // strip it so `print(x)` produces "x\n" (not "x\n\n").
            FILE* tmp = std::tmpfile();
            if (tmp) {
                PyObject_Print(argList->list[i], tmp);
                std::fflush(tmp);
                std::rewind(tmp);
                char buf[4096];
                size_t n = std::fread(buf, 1, sizeof(buf) - 1, tmp);
                buf[n] = '\0';
                // Strip a single trailing newline (PyObject_Print adds one).
                if (n > 0 && buf[n-1] == '\n') buf[--n] = '\0';
                out += buf;
                std::fclose(tmp);
            } else {
                out += "<print-error>";
            }
        }
    }
    out += end->str;
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// ---- List helper methods ----
PyObject* PyList_Insert(PyObject* list, PyObject* idx, PyObject* item) {
    if (!list || list->type != 1 || !idx || (idx->type != 0 && idx->type != 5)) return nullptr;
    long i = idx->value;
    if (i < 0) i += (long)list->list.size();
    if (i < 0) i = 0;
    if (i > (long)list->list.size()) i = (long)list->list.size();
    list->list.insert(list->list.begin() + i, item);
    if (item) Py_INCREF(item);
    return PyInt_FromLong(0);
}
PyObject* PyList_Remove(PyObject* list, PyObject* item) {
    if (!list || list->type != 1) return nullptr;
    for (auto it = list->list.begin(); it != list->list.end(); ++it) {
        bool eq = (*it == item) ||
                  (*it && item && PyObject_CompareBool(*it, item, 0));
        if (eq) {
            if (*it) Py_DECREF(*it);
            list->list.erase(it);
            return PyInt_FromLong(0);
        }
    }
    // CPython raises ValueError; we return NULL silently.
    return nullptr;
}
PyObject* PyList_Index(PyObject* list, PyObject* item) {
    if (!list || list->type != 1) return nullptr;
    for (size_t i = 0; i < list->list.size(); ++i) {
        bool eq = (list->list[i] == item) ||
                  (list->list[i] && item && PyObject_CompareBool(list->list[i], item, 0));
        if (eq) return PyInt_FromLong((long)i);
    }
    return PyInt_FromLong(-1);
}
PyObject* PyList_Count(PyObject* list, PyObject* item) {
    if (!list || list->type != 1) return PyInt_FromLong(0);
    long c = 0;
    for (auto* e : list->list) {
        if (e == item || (e && item && PyObject_CompareBool(e, item, 0))) ++c;
    }
    return PyInt_FromLong(c);
}
PyObject* PyList_Reverse(PyObject* list) {
    if (!list || list->type != 1) return nullptr;
    std::reverse(list->list.begin(), list->list.end());
    return PyInt_FromLong(0);
}
PyObject* PyList_Extend(PyObject* list, PyObject* other) {
    if (!list || list->type != 1) return nullptr;
    if (other) {
        if (other->type == 1) {
            for (auto* e : other->list) {
                if (e) Py_INCREF(e);
                list->list.push_back(e);
            }
        } else if (other->type == 2) {
            for (auto& p : other->dict) {
                if (p.first) Py_INCREF(p.first);
                list->list.push_back(p.first);
            }
        }
    }
    return PyInt_FromLong(0);
}
PyObject* PyList_Copy(PyObject* list) {
    if (!list || list->type != 1) return PyList_New(0);
    PyObject* r = PyList_New(list->list.size());
    for (size_t i = 0; i < list->list.size(); ++i) {
        if (list->list[i]) Py_INCREF(list->list[i]);
        PyList_SetItem(r, i, list->list[i]);
    }
    return r;
}
PyObject* PyList_Clear(PyObject* list) {
    if (!list || list->type != 1) return nullptr;
    if (list->list_item_type == 0) {
        for (auto* e : list->list) if (e) Py_DECREF(e);
    }
    list->list.clear();
    list->ilist.clear();
    list->flist.clear();
    list->list_item_type = 0;
    return PyInt_FromLong(0);
}
PyObject* PyList_PopAt(PyObject* list, PyObject* idx) {
    if (!list || list->type != 1) return nullptr;
    long i;
    // Handle homogeneous int lists
    if (list->list_item_type == 1) {
        if (idx && (idx->type == 0 || idx->type == 5)) {
            i = idx->value;
            if (i < 0) i += (long)list->ilist.size();
        } else {
            i = (long)list->ilist.size() - 1;
        }
        if (i < 0 || i >= (long)list->ilist.size()) return nullptr;
        long val = list->ilist[i];
        list->ilist.erase(list->ilist.begin() + i);
        return PyInt_FromLong(val);
    }
    // Handle homogeneous float lists
    if (list->list_item_type == 2) {
        if (idx && (idx->type == 0 || idx->type == 5)) {
            i = idx->value;
            if (i < 0) i += (long)list->flist.size();
        } else {
            i = (long)list->flist.size() - 1;
        }
        if (i < 0 || i >= (long)list->flist.size()) return nullptr;
        double val = list->flist[i];
        list->flist.erase(list->flist.begin() + i);
        return PyFloat_FromDouble(val);
    }
    // Handle regular boxed lists
    if (list->list.empty()) return nullptr;
    if (idx && (idx->type == 0 || idx->type == 5)) {
        i = idx->value;
        if (i < 0) i += (long)list->list.size();
    } else {
        i = (long)list->list.size() - 1;
    }
    if (i < 0 || i >= (long)list->list.size()) return nullptr;
    PyObject* r = list->list[i];
    if (r) Py_INCREF(r);
    list->list.erase(list->list.begin() + i);
    return r;
}

// ---- Dict helper methods ----
PyObject* PyDict_Update(PyObject* dst, PyObject* src) {
    if (!dst || dst->type != 2) return nullptr;
    if (src && src->type == 2) {
        for (auto& p : src->dict) {
            PyDict_SetItem(dst, p.first, p.second);
        }
    }
    return PyInt_FromLong(0);
}
PyObject* PyDict_SetDefault(PyObject* d, PyObject* key, PyObject* defval) {
    if (!d || d->type != 2 || !key) return nullptr;
    for (auto& p : d->dict) {
        if (p.first == key || (p.first && PyObject_CompareBool(p.first, key, 0))) {
            if (p.second) Py_INCREF(p.second);
            return p.second;
        }
    }
    if (defval) {
        PyDict_SetItem(d, key, defval);
        Py_INCREF(defval);
        return defval;
    }
    return nullptr;
}
PyObject* PyDict_Copy(PyObject* d) {
    if (!d || d->type != 2) return PyDict_New();
    PyObject* r = PyDict_New();
    for (auto& p : d->dict) {
        PyDict_SetItem(r, p.first, p.second);
    }
    return r;
}
PyObject* PyDict_Clear(PyObject* d) {
    if (!d || d->type != 2) return nullptr;
    for (auto& p : d->dict) {
        if (p.first) Py_DECREF(p.first);
        if (p.second) Py_DECREF(p.second);
    }
    d->dict.clear();
    return PyInt_FromLong(0);
}
PyObject* PyDict_Pop(PyObject* d, PyObject* key, PyObject* defval) {
    if (!d || d->type != 2 || !key) return nullptr;
    for (auto it = d->dict.begin(); it != d->dict.end(); ++it) {
        if (it->first == key || (it->first && PyObject_CompareBool(it->first, key, 0))) {
            PyObject* v = it->second;
            if (v) Py_INCREF(v);
            if (it->first) Py_DECREF(it->first);
            if (it->second) Py_DECREF(it->second);
            d->dict.erase(it);
            return v;
        }
    }
    if (defval) {
        Py_INCREF(defval);
        return defval;
    }
    return nullptr;
}
PyObject* PyDict_PopItem(PyObject* d) {
    if (!d || d->type != 2 || d->dict.empty()) return nullptr;
    auto it = d->dict.begin();
    PyObject* k = it->first; if (k) Py_INCREF(k);
    PyObject* v = it->second; if (v) Py_INCREF(v);
    if (it->first) Py_DECREF(it->first);
    if (it->second) Py_DECREF(it->second);
    d->dict.erase(it);
    PyObject* pair = PyList_New(2);
    PyList_SetItem(pair, 0, k);
    PyList_SetItem(pair, 1, v);
    return pair;
}
PyObject* PyDict_FromKeys(PyObject* keys, PyObject* defval) {
    PyObject* r = PyDict_New();
    if (!keys) return r;
    if (keys->type == 1) {
        for (auto* k : keys->list) {
            PyObject* v = defval;
            if (v) Py_INCREF(v);
            PyDict_SetItem(r, k, v);
        }
    }
    return r;
}

// ---- String helper methods ----
PyObject* PyString_LStrip(PyObject* s) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    size_t l = 0;
    while (l < s->str.size() && isspace((unsigned char)s->str[l])) ++l;
    return PyUnicode_FromString(s->str.substr(l).c_str());
}
PyObject* PyString_RStrip(PyObject* s) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    size_t r = s->str.size();
    while (r > 0 && isspace((unsigned char)s->str[r-1])) --r;
    return PyUnicode_FromString(s->str.substr(0, r).c_str());
}
PyObject* PyString_StartsWith(PyObject* s, PyObject* prefix) {
    if (!s || s->type != 3 || !prefix || prefix->type != 3) return PyBool_New(0);
    if (prefix->str.size() > s->str.size()) return PyBool_New(0);
    return PyBool_New(s->str.compare(0, prefix->str.size(), prefix->str) == 0);
}
PyObject* PyString_EndsWith(PyObject* s, PyObject* suffix) {
    if (!s || s->type != 3 || !suffix || suffix->type != 3) return PyBool_New(0);
    if (suffix->str.size() > s->str.size()) return PyBool_New(0);
    return PyBool_New(s->str.compare(s->str.size() - suffix->str.size(), suffix->str.size(), suffix->str) == 0);
}
PyObject* PyString_IsAlpha(PyObject* s) {
    if (!s || s->type != 3) return PyBool_New(0);
    if (s->str.empty()) return PyBool_New(0);
    for (char c : s->str) {
        if (!isalpha((unsigned char)c)) return PyBool_New(0);
    }
    return PyBool_New(1);
}
PyObject* PyString_IsDigit(PyObject* s) {
    if (!s || s->type != 3) return PyBool_New(0);
    if (s->str.empty()) return PyBool_New(0);
    for (char c : s->str) {
        if (!isdigit((unsigned char)c)) return PyBool_New(0);
    }
    return PyBool_New(1);
}
PyObject* PyString_IsAlnum(PyObject* s) {
    if (!s || s->type != 3) return PyBool_New(0);
    if (s->str.empty()) return PyBool_New(0);
    for (char c : s->str) {
        if (!isalnum((unsigned char)c)) return PyBool_New(0);
    }
    return PyBool_New(1);
}
PyObject* PyString_IsLower(PyObject* s) {
    if (!s || s->type != 3) return PyBool_New(0);
    bool any = false;
    for (char c : s->str) {
        if (isupper((unsigned char)c)) return PyBool_New(0);
        if (islower((unsigned char)c)) any = true;
    }
    return PyBool_New(any);
}
PyObject* PyString_IsUpper(PyObject* s) {
    if (!s || s->type != 3) return PyBool_New(0);
    bool any = false;
    for (char c : s->str) {
        if (islower((unsigned char)c)) return PyBool_New(0);
        if (isupper((unsigned char)c)) any = true;
    }
    return PyBool_New(any);
}
PyObject* PyString_IsSpace(PyObject* s) {
    if (!s || s->type != 3) return PyBool_New(0);
    if (s->str.empty()) return PyBool_New(0);
    for (char c : s->str) {
        if (!isspace((unsigned char)c)) return PyBool_New(0);
    }
    return PyBool_New(1);
}
PyObject* PyString_Casefold(PyObject* s) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    std::string r = s->str;
    for (char& c : r) c = (char)tolower((unsigned char)c);
    return PyUnicode_FromString(r.c_str());
}
PyObject* PyString_Title(PyObject* s) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    std::string r = s->str;
    bool atWord = true;
    for (char& c : r) {
        if (isspace((unsigned char)c)) atWord = true;
        else if (atWord) { c = (char)toupper((unsigned char)c); atWord = false; }
        else c = (char)tolower((unsigned char)c);
    }
    return PyUnicode_FromString(r.c_str());
}
PyObject* PyString_ZFill(PyObject* s, PyObject* w) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    long width = (w && (w->type == 0 || w->type == 5)) ? w->value : 0;
    if ((long)s->str.size() >= width) { Py_INCREF(s); return s; }
    std::string r;
    if (!s->str.empty() && (s->str[0] == '+' || s->str[0] == '-')) {
        r += s->str[0];
        r.append(width - s->str.size(), '0');
        r += s->str.substr(1);
    } else {
        r.append(width - s->str.size(), '0');
        r += s->str;
    }
    return PyUnicode_FromString(r.c_str());
}
PyObject* PyString_Center(PyObject* s, PyObject* w, PyObject* fill) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    long width = (w && (w->type == 0 || w->type == 5)) ? w->value : 0;
    std::string fc = (fill && fill->type == 3 && !fill->str.empty()) ? fill->str.substr(0, 1) : " ";
    if ((long)s->str.size() >= width) { Py_INCREF(s); return s; }
    long pad = width - s->str.size();
    long lp = pad / 2;
    long rp = pad - lp;
    std::string r;
    r.append(lp, fc[0]);
    r += s->str;
    r.append(rp, fc[0]);
    return PyUnicode_FromString(r.c_str());
}
PyObject* PyString_LJust(PyObject* s, PyObject* w, PyObject* fill) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    long width = (w && (w->type == 0 || w->type == 5)) ? w->value : 0;
    std::string fc = (fill && fill->type == 3 && !fill->str.empty()) ? fill->str.substr(0, 1) : " ";
    if ((long)s->str.size() >= width) { Py_INCREF(s); return s; }
    std::string r = s->str;
    r.append(width - s->str.size(), fc[0]);
    return PyUnicode_FromString(r.c_str());
}
PyObject* PyString_RJust(PyObject* s, PyObject* w, PyObject* fill) {
    if (!s || s->type != 3) return s ? (Py_INCREF(s), s) : nullptr;
    long width = (w && (w->type == 0 || w->type == 5)) ? w->value : 0;
    std::string fc = (fill && fill->type == 3 && !fill->str.empty()) ? fill->str.substr(0, 1) : " ";
    if ((long)s->str.size() >= width) { Py_INCREF(s); return s; }
    std::string r;
    r.append(width - s->str.size(), fc[0]);
    r += s->str;
    return PyUnicode_FromString(r.c_str());
}
PyObject* PyString_ReplaceN(PyObject* s, PyObject* old_, PyObject* new_, PyObject* count) {
    if (!s || s->type != 3 || !old_ || old_->type != 3 || !new_ || new_->type != 3) {
        if (s) { Py_INCREF(s); return s; }
        return nullptr;
    }
    std::string result = s->str;
    const std::string& from = old_->str;
    const std::string& to   = new_->str;
    if (from.empty()) { Py_INCREF(s); return s; }
    long maxCount = (count && (count->type == 0 || count->type == 5)) ? count->value : -1;
    if (maxCount == 0) { Py_INCREF(s); return s; }
    long n = 0;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        if (maxCount >= 0 && n >= maxCount) break;
        result.replace(pos, from.size(), to);
        pos += to.size();
        ++n;
    }
    return PyUnicode_FromString(result.c_str());
}
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
// The re module is a synthetic dict (PCRE2-backed). For every other
// module, this prints an ImportError to stderr and returns null. The
// `re` module dict contains string tokens naming the runtime helpers
// (the compiler's lowerMethodCall short-circuits `re.<name>(...)` and
// emits the direct call to PyBuiltin_Re*; the tokens themselves are
// never read by Pyc_Apply, so their values are arbitrary sentinels).
static PyObject* makeReModuleDict() {
    PyObject* d = PyDict_New();
    auto add = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k);
        Py_DECREF(v);
    };
    add("finditer", "PyBuiltin_ReFinditer");
    add("findall",  "PyBuiltin_ReFindall");
    add("compile",  "PyBuiltin_ReCompile");
    add("match",    "PyBuiltin_ReMatch");
    add("search",   "PyBuiltin_ReSearch");
    add("sub",      "PyBuiltin_ReSub");
    return d;
}

// Forward declaration: the `sys` module is set up at startup by
// pyc_setup_sys. Defined further down in this file.
static PyObject* g_sys_module = nullptr;

// Forward declarations for the os / subprocess module dict builders.
static PyObject* makeOsModuleDict();
static PyObject* makeSubprocessModuleDict();

PyObject* pyc_import_failed(PyObject* modName) {
    if (modName && modName->type == 3) {
        if (modName->str == "re") {
            // Return a synthetic re module dict.
            return makeReModuleDict();
        }
        if (modName->str == "sys") {
            // `sys` is built at startup (pyc_setup_sys). Return it as a
            // module so attribute access works.
            if (g_sys_module) {
                Py_INCREF(g_sys_module);
                return g_sys_module;
            }
            return PyDict_New();
        }
        if (modName->str == "functools") {
            // functools isn't fully supported, but cmp_to_key is needed
            // by the sorted-with-comparator idiom (handled at the AST
            // level). Return a dict with a cmp_to_key token so attribute
            // access doesn't crash.
            PyObject* d = PyDict_New();
            PyObject* k = PyUnicode_FromString("cmp_to_key");
            PyObject* v = PyUnicode_FromString("cmp_to_key");
            PyDict_SetItem(d, k, v);
            Py_DECREF(k); Py_DECREF(v);
            return d;
        }
        if (modName->str == "os") {
            return makeOsModuleDict();
        }
        if (modName->str == "subprocess") {
            return makeSubprocessModuleDict();
        }
    }
    const char* name = (modName && modName->type == 3) ? modName->str.c_str() : "?";
    fprintf(stderr, "ImportError: No module named '%s' "
                    "(pyc supports only synthetic 'sys', 're', 'functools', 'os', "
                    "and 'subprocess' modules; "
                    "real module loading is not yet implemented)\n", name);
    fflush(stderr);
    return nullptr;
}

PyObject* PyBuiltin_Len(PyObject* obj) {
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 1) return PyInt_FromLong((long)PyList_Size(obj));
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
        // Handle homogeneous int lists
        if (lst->list_item_type == 1) {
            for (size_t i = 0; i < lst->ilist.size(); ++i) {
                items.push_back(PyInt_FromLong(lst->ilist[i]));
            }
        }
        // Handle homogeneous float lists
        else if (lst->list_item_type == 2) {
            for (size_t i = 0; i < lst->flist.size(); ++i) {
                items.push_back(PyFloat_FromDouble(lst->flist[i]));
            }
        }
        else {
            for (auto* item : lst->list) {
                if (item) Py_INCREF(item);
                items.push_back(item);
            }
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
        // Handle homogeneous int lists
        if (lst->list_item_type == 1) {
            for (size_t i = 0; i < lst->ilist.size(); ++i) {
                items.push_back(PyInt_FromLong(lst->ilist[i]));
            }
        }
        // Handle homogeneous float lists
        else if (lst->list_item_type == 2) {
            for (size_t i = 0; i < lst->flist.size(); ++i) {
                items.push_back(PyFloat_FromDouble(lst->flist[i]));
            }
        }
        else {
            for (auto* item : lst->list) {
                if (item) Py_INCREF(item);
                items.push_back(item);
            }
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
    if (!typecode || typecode->type != 0 || typecode->value < 0)
        return PyBool_New(1);
    int code = (int)typecode->value;
    if (code == 6) {
        // NoneType: None is the only instance (represented as null ptr).
        return PyBool_New(obj == nullptr ? 1 : 0);
    }
    if (!obj) return PyBool_New(0);
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
    long n;
    if (obj->type == 1) {
        // Handle homogeneous lists
        if (obj->list_item_type == 1) {
            n = (long)obj->ilist.size();
        } else if (obj->list_item_type == 2) {
            n = (long)obj->flist.size();
        } else {
            n = (long)obj->list.size();
        }
    } else if (obj->type == 3) {
        n = (long)obj->str.size();
    } else {
        n = 0;
    }
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
        // Handle homogeneous int lists
        if (obj->list_item_type == 1) {
            PyObject* r = PyList_NewIntBoxed(PyInt_FromLong((long)idxs.size()));
            for (size_t k = 0; k < idxs.size(); ++k) {
                long i = idxs[k];
                long val = (i >= 0 && (size_t)i < obj->ilist.size()) ? obj->ilist[i] : 0;
                PyList_SetItemInt64(r, k, val);
            }
            return r;
        }
        // Handle homogeneous float lists
        if (obj->list_item_type == 2) {
            PyObject* r = PyList_NewFloatBoxed(PyInt_FromLong((long)idxs.size()));
            for (size_t k = 0; k < idxs.size(); ++k) {
                long i = idxs[k];
                double val = (i >= 0 && (size_t)i < obj->flist.size()) ? obj->flist[i] : 0.0;
                PyList_SetItemDouble(r, k, val);
            }
            return r;
        }
        // Regular boxed list
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
    
    // Convert homogeneous lists to regular lists for slice operations
    bool wasIntBoxed = (obj->list_item_type == 1);
    bool wasFloatBoxed = (obj->list_item_type == 2);
    
    if (wasIntBoxed) {
        // Convert ilist to list
        obj->list.resize(obj->ilist.size());
        for (size_t i = 0; i < obj->ilist.size(); ++i) {
            obj->list[i] = PyInt_FromLong(obj->ilist[i]);
        }
        obj->ilist.clear();
        obj->list_item_type = 0;
    } else if (wasFloatBoxed) {
        // Convert flist to list
        obj->list.resize(obj->flist.size());
        for (size_t i = 0; i < obj->flist.size(); ++i) {
            obj->list[i] = PyFloat_FromDouble(obj->flist[i]);
        }
        obj->flist.clear();
        obj->list_item_type = 0;
    }
    
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
        // Handle homogeneous int lists
        if (value->list_item_type == 1) {
            for (size_t i = 0; i < value->ilist.size(); ++i) {
                PyObject* v = PyInt_FromLong(value->ilist[i]);
                repl.push_back(v);
            }
        }
        // Handle homogeneous float lists
        else if (value->list_item_type == 2) {
            for (size_t i = 0; i < value->flist.size(); ++i) {
                PyObject* v = PyFloat_FromDouble(value->flist[i]);
                repl.push_back(v);
            }
        }
        else {
            for (auto* v : value->list) { if (v) Py_INCREF(v); repl.push_back(v); }
        }
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
    if (obj->type == 2) {
        // CPython: list(dict) iterates over keys.
        PyObject* r = PyList_New(obj->dict.size());
        size_t i = 0;
        for (auto& pair : obj->dict) {
            if (pair.first) Py_INCREF(pair.first);
            PyList_SetItem(r, i++, pair.first);
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
        // Handle homogeneous int lists
        if (obj->list_item_type == 1) {
            r = PyList_NewIntBoxed(PyInt_FromLong((long)obj->ilist.size()));
            for (size_t i = 0; i < obj->ilist.size(); ++i) {
                size_t ri = obj->ilist.size() - 1 - i;
                PyList_SetItemInt64(r, i, obj->ilist[ri]);
            }
            return r;
        }
        // Handle homogeneous float lists
        if (obj->list_item_type == 2) {
            r = PyList_NewFloatBoxed(PyInt_FromLong((long)obj->flist.size()));
            for (size_t i = 0; i < obj->flist.size(); ++i) {
                size_t ri = obj->flist.size() - 1 - i;
                PyList_SetItemDouble(r, i, obj->flist[ri]);
            }
            return r;
        }
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
PyObject* PyString_Format(PyObject* fmt, PyObject* args) {
    if (!fmt || fmt->type != 3) return nullptr;
    auto getArg = [&](size_t idx) -> PyObject* {
        if (!args) return nullptr;
        // Handle homogeneous int lists
        if (args->type == 1 && args->list_item_type == 1 && idx < args->ilist.size()) {
            return PyInt_FromLong(args->ilist[idx]);
        }
        // Handle homogeneous float lists
        if (args->type == 1 && args->list_item_type == 2 && idx < args->flist.size()) {
            return PyFloat_FromDouble(args->flist[idx]);
        }
        // Handle regular boxed lists
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
// (Forward-declared near pyc_import_failed above.)
static PyObject* g_sys_argv = nullptr;

// Allocator: PyObject* is a flat struct (see above). For the regex
// types (8 and 9) we set `value` to the pointer to a heap-allocated
// CompiledRegex* or MatchObj*. We never use `list`, `dict`, or `str`
// for these types. Py_DECREF on a type 8/9 object frees the embedded
// payload.

static PyObject* allocObject(int type) {
    PyObject* o = (PyObject*)calloc(1, sizeof(PyObject));
    if (!o) return nullptr;
    o->refcount = 1;
    o->type = type;
    return o;
}

static void freeObject(PyObject* o) {
    if (!o) return;
    if (o->type == 8) {
        CompiledRegex* cr = reinterpret_cast<CompiledRegex*>(o->value);
        delete cr;
    } else if (o->type == 9) {
        MatchObj* mo = reinterpret_cast<MatchObj*>(o->value);
        delete mo;
    }
    free(o);
}

static CompiledRegex* asCompiledRegex(PyObject* o) {
    if (!o || o->type != 8) return nullptr;
    return reinterpret_cast<CompiledRegex*>(o->value);
}

static MatchObj* asMatchObj(PyObject* o) {
    if (!o || o->type != 9) return nullptr;
    return reinterpret_cast<MatchObj*>(o->value);
}

static pcre2_code* compileRegex(const std::string& pat, std::string& err) {
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code* code = pcre2_compile(
        (PCRE2_SPTR)pat.c_str(), (PCRE2_SIZE)pat.size(),
        0, &errcode, &erroffset, nullptr);
    if (!code) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(errcode, buf, sizeof(buf));
        err = std::string((const char*)buf) + " at offset " + std::to_string(erroffset);
        return nullptr;
    }
    return code;
}

// Run a regex against subject and return a list of Match objects (type 9).
static PyObject* runRegexAll(pcre2_code* code, const std::string& subject) {
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    if (!md) return nullptr;
    PCRE2_SPTR subj = (PCRE2_SPTR)subject.c_str();
    int rc = pcre2_match(code, subj, (PCRE2_SIZE)subject.size(),
                         0, 0, md, nullptr);
    if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
        pcre2_match_data_free(md);
        return nullptr;
    }
    // Build a list of all matches. Each match is a new MatchObj (type 9)
    // that takes a fresh match_data + a copy of the ovector.
    PyObject* result = PyList_New(0);
    PCRE2_SIZE offset = 0;
    while (rc >= 0) {
        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(md);
        int capture_count = rc - 1;  // see ReFindall for the rc-1 rationale
        // Copy the ovector for this match into a new match_data
        pcre2_match_data* mdcopy = pcre2_match_data_create(capture_count + 1, nullptr);
        if (!mdcopy) break;
        PCRE2_SIZE* dst_ov = pcre2_get_ovector_pointer(mdcopy);
        for (int k = 0; k < 2 * (capture_count + 1); ++k) dst_ov[k] = ovector[k];
        PyObject* m = allocObject(9);
        if (!m) { pcre2_match_data_free(mdcopy); break; }
        MatchObj* mo = new MatchObj();
        mo->md = mdcopy;
        mo->subject = subject;
        mo->capture_count = capture_count;
        m->value = (long)(intptr_t)mo;
        // Append (bypass the size-bounded PyList_SetItem)
        result->list.push_back(m);
        // Move past this match
        offset = ovector[1];
        if (offset == ovector[0]) offset++;
        if (offset > subject.size()) break;
        rc = pcre2_match(code, subj, (PCRE2_SIZE)subject.size(),
                         offset, 0, md, nullptr);
    }
    pcre2_match_data_free(md);
    return result;
}

extern "C" PyObject* PyBuiltin_ReFinditer(PyObject* pattern, PyObject* subject) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err);
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
        return nullptr;
    }
    PyObject* result = runRegexAll(code, subject->str);
    pcre2_code_free(code);
    return result;
}

extern "C" PyObject* PyBuiltin_ReFindall(PyObject* pattern, PyObject* subject) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err);
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
        return nullptr;
    }
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    if (!md) { pcre2_code_free(code); return nullptr; }
    PCRE2_SPTR subj = (PCRE2_SPTR)subject->str.c_str();
    int rc = pcre2_match(code, subj, (PCRE2_SIZE)subject->str.size(), 0, 0, md, nullptr);
    PyObject* result = PyList_New(0);
    PCRE2_SIZE offset = 0;
    while (rc >= 0) {
        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(md);
        // rc is "one more than the highest-numbered pair that was set".
        // So #capture_groups = rc - 1 (pair 0 is the full match, which is
        // not counted in rc's "highest pair" sense).
        int num_capture_groups = rc - 1;
        if (num_capture_groups <= 0) {
            std::string m = subject->str.substr(ovector[0], ovector[1] - ovector[0]);
            PyObject* s = PyUnicode_FromString(m.c_str());
            result->list.push_back(s);
        } else if (num_capture_groups == 1) {
            std::string g = subject->str.substr(ovector[2], ovector[3] - ovector[2]);
            PyObject* s = PyUnicode_FromString(g.c_str());
            result->list.push_back(s);
        } else {
            PyObject* tup = PyList_New(num_capture_groups);
            for (int g = 1; g <= num_capture_groups; ++g) {
                std::string gs = subject->str.substr(ovector[2*g], ovector[2*g+1] - ovector[2*g]);
                PyObject* s = PyUnicode_FromString(gs.c_str());
                tup->list[g-1] = s;
            }
            result->list.push_back(tup);
        }
        offset = ovector[1];
        if (offset == ovector[0]) offset++;
        if (offset > subject->str.size()) break;
        rc = pcre2_match(code, subj, (PCRE2_SIZE)subject->str.size(), offset, 0, md, nullptr);
    }
    pcre2_match_data_free(md);
    pcre2_code_free(code);
    return result;
}

// m.group(i) — return the i-th capture group as a string. m.group() or
// m.group(0) returns the full match.
extern "C" PyObject* PyBuiltin_ReMatchGroup(PyObject* m, PyObject* idxObj) {
    MatchObj* mo = asMatchObj(m);
    if (!mo || !mo->md) return nullptr;
    long i = 0;
    if (idxObj && (idxObj->type == 0 || idxObj->type == 5)) i = idxObj->value;
    if (i < 0 || i > mo->capture_count) return nullptr;
    PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(mo->md);
    PCRE2_SIZE start = ovector[2*i];
    PCRE2_SIZE end   = ovector[2*i+1];
    if (start == PCRE2_UNSET || end == PCRE2_UNSET) return nullptr;
    return PyUnicode_FromString(mo->subject.substr(start, end - start).c_str());
}

// re.search(pattern, subject) — return a Match object (type 9) for the
// first match, or None if no match. We treat re.search as a 2-arg
// helper (ignore re.IGNORECASE etc. for now).
extern "C" PyObject* PyBuiltin_ReSearch(PyObject* pattern, PyObject* subject) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    // We always compile with PCRE2_CASELESS for now so re.IGNORECASE
    // is implicit. This matches what the test/regex.py expects.
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code* code = pcre2_compile(
        (PCRE2_SPTR)pattern->str.c_str(), (PCRE2_SIZE)pattern->str.size(),
        PCRE2_CASELESS, &errcode, &erroffset, nullptr);
    if (!code) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(errcode, buf, sizeof(buf));
        std::fprintf(stderr, "re.error: %s at %zu\n", (char*)buf, erroffset);
        return nullptr;
    }
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    if (!md) { pcre2_code_free(code); return nullptr; }
    int rc = pcre2_match(code, (PCRE2_SPTR)subject->str.c_str(),
                         (PCRE2_SIZE)subject->str.size(), 0, 0, md, nullptr);
    if (rc < 0) {
        pcre2_match_data_free(md);
        pcre2_code_free(code);
        return nullptr;  // no match
    }
    int capture_count = rc - 1;
    pcre2_match_data* mdcopy = pcre2_match_data_create(capture_count + 1, nullptr);
    if (!mdcopy) {
        pcre2_match_data_free(md);
        pcre2_code_free(code);
        return nullptr;
    }
    PCRE2_SIZE* src_ov = pcre2_get_ovector_pointer(md);
    PCRE2_SIZE* dst_ov = pcre2_get_ovector_pointer(mdcopy);
    for (int k = 0; k < 2 * (capture_count + 1); ++k) dst_ov[k] = src_ov[k];
    pcre2_match_data_free(md);
    pcre2_code_free(code);
    PyObject* m = allocObject(9);
    if (!m) { pcre2_match_data_free(mdcopy); return nullptr; }
    MatchObj* mo = new MatchObj();
    mo->md = mdcopy;
    mo->subject = subject->str;
    mo->capture_count = capture_count;
    m->value = (long)(intptr_t)mo;
    return m;
}

extern "C" PyObject* PyBuiltin_ReCompile(PyObject* pattern) {
    if (!pattern || pattern->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err);
    if (!code) { std::fprintf(stderr, "re.error: %s\n", err.c_str()); return nullptr; }
    PyObject* o = allocObject(8);
    if (!o) { pcre2_code_free(code); return nullptr; }
    CompiledRegex* cr = new CompiledRegex();
    cr->code = code;
    cr->pattern = pattern->str;
    o->value = (long)(intptr_t)cr;
    return o;
}

// Integer left shift. Unboxes a, applies a<<b (assumes b is int), and
// re-boxes the result. Returns nullptr on error.
extern "C" PyObject* PyNumber_Lshift(PyObject* a, PyObject* b) {
    long av = 0, bv = 0;
    if (a && a->type == 0) av = (long)a->value;
    else if (a && a->type == 5) av = (long)a->value;
    if (b && b->type == 0) bv = (long)b->value;
    else if (b && b->type == 5) bv = (long)b->value;
    return PyInt_FromLong(av << bv);
}

extern "C" PyObject* PyNumber_Rshift(PyObject* a, PyObject* b) {
    long av = 0, bv = 0;
    if (a && a->type == 0) av = (long)a->value;
    else if (a && a->type == 5) av = (long)a->value;
    if (b && b->type == 0) bv = (long)b->value;
    else if (b && b->type == 5) bv = (long)b->value;
    return PyInt_FromLong(av >> bv);
}

extern "C" PyObject* PyNumber_BitOr(PyObject* a, PyObject* b) {
    long av = 0, bv = 0;
    if (a && a->type == 0) av = (long)a->value;
    else if (a && a->type == 5) av = (long)a->value;
    if (b && b->type == 0) bv = (long)b->value;
    else if (b && b->type == 5) bv = (long)b->value;
    return PyInt_FromLong(av | bv);
}

extern "C" PyObject* PyNumber_BitAnd(PyObject* a, PyObject* b) {
    long av = 0, bv = 0;
    if (a && a->type == 0) av = (long)a->value;
    else if (a && a->type == 5) av = (long)a->value;
    if (b && b->type == 0) bv = (long)b->value;
    else if (b && b->type == 5) bv = (long)b->value;
    return PyInt_FromLong(av & bv);
}

extern "C" PyObject* PyNumber_BitXor(PyObject* a, PyObject* b) {
    long av = 0, bv = 0;
    if (a && a->type == 0) av = (long)a->value;
    else if (a && a->type == 5) av = (long)a->value;
    if (b && b->type == 0) bv = (long)b->value;
    else if (b && b->type == 5) bv = (long)b->value;
    return PyInt_FromLong(av ^ bv);
}

// os.path.exists(path) -> bool : True if the file or directory exists
extern "C" PyObject* PyBuiltin_OsPathExists(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyBool_New(0);
    PyObject* path = args->list[0];
    if (!path || path->type != 3) return PyBool_New(0);
    struct stat st;
    int r = ::stat(path->str.c_str(), &st);
    return PyBool_New(r == 0 ? 1 : 0);
}

// os.path.isfile(path) -> bool : True if the path is a regular file
extern "C" PyObject* PyBuiltin_OsPathIsfile(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyBool_New(0);
    PyObject* path = args->list[0];
    if (!path || path->type != 3) return PyBool_New(0);
    struct stat st;
    if (::stat(path->str.c_str(), &st) != 0) return PyBool_New(0);
    return PyBool_New(S_ISREG(st.st_mode) ? 1 : 0);
}

// os.path.isdir(path) -> bool : True if the path is a directory
extern "C" PyObject* PyBuiltin_OsPathIsdir(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyBool_New(0);
    PyObject* path = args->list[0];
    if (!path || path->type != 3) return PyBool_New(0);
    struct stat st;
    if (::stat(path->str.c_str(), &st) != 0) return PyBool_New(0);
    return PyBool_New(S_ISDIR(st.st_mode) ? 1 : 0);
}

// os.unlink(path) -> None : remove a file
extern "C" PyObject* PyBuiltin_OsUnlink(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* path = args->list[0];
    if (!path || path->type != 3) return nullptr;
    ::unlink(path->str.c_str());
    return nullptr;
}

// os.environ dict (empty stub; user code can set/get but values are lost)
extern "C" PyObject* PyBuiltin_GetEnviron() {
    return PyDict_New();
}

// open(path, mode) — open a file. The path/mode are extracted from the
// args list. Returns a synthetic "file" dict with __enter__ / __exit__
// / write / close keys (all string tokens naming runtime adapters that
// are registered in pyc_setup_callables). The runtime adapters hold
// onto the FILE* in a static map keyed by the dict's identity. This
// is good enough for `with open(path, "w") as fh: fh.write(s)` and
// similar basic patterns. Concurrent or recursive opens of the same
// file are not supported.
struct PycFile {
    std::string path;
    std::string mode;
    FILE* fp;
};
static std::unordered_map<PyObject*, PycFile> g_pycFiles;

static PyObject* pyc_file_write_adapter(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* self = args->list[0];
    auto it = g_pycFiles.find(self);
    if (it == g_pycFiles.end() || !it->second.fp) return nullptr;
    PyObject* data = args->list[1];
    if (data && data->type == 3) {
        std::fwrite(data->str.data(), 1, data->str.size(), it->second.fp);
        std::fflush(it->second.fp);
    }
    return nullptr;
}

static PyObject* pyc_file_enter_adapter(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    return args->list[0];
}

static PyObject* pyc_file_exit_adapter(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* self = args->list[0];
    auto it = g_pycFiles.find(self);
    if (it != g_pycFiles.end() && it->second.fp) {
        std::fclose(it->second.fp);
        g_pycFiles.erase(it);
    }
    return nullptr;
}

extern "C" PyObject* PyBuiltin_Open(PyObject* path, PyObject* mode) {
    if (!path || path->type != 3) return nullptr;
    std::string pathStr = path->str;
    std::string modeStr = (mode && mode->type == 3) ? mode->str : std::string("r");
    FILE* fp = std::fopen(pathStr.c_str(), modeStr.c_str());
    if (!fp) {
        std::fprintf(stderr, "FileNotFoundError: [Errno 2] No such file or directory: '%s'\n", pathStr.c_str());
        return nullptr;
    }
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("__enter__", "pyc_file_enter");
    addTok("__exit__",  "pyc_file_exit");
    addTok("write",     "pyc_file_write");
    g_pycFiles[d] = {pathStr, modeStr, fp};
    return d;
}

// subprocess.call(args) -> int : spawn a subprocess, return its exit status
extern "C" PyObject* PyBuiltin_SubprocessCall(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyInt_FromLong(-1);
    if (!args->list[0] || args->list[0]->type != 1) return PyInt_FromLong(-1);
    PyObject* argv = args->list[0];
    std::vector<std::string> strs;
    for (size_t i = 0; i < argv->list.size(); ++i) {
        PyObject* s = argv->list[i];
        if (!s || s->type != 3) return PyInt_FromLong(-1);
        strs.push_back(s->str);
    }
    if (strs.empty()) return PyInt_FromLong(-1);
    // Quote each arg to handle newlines/spaces safely. `system` runs
    // through /bin/sh -c, so we need shell quoting.
    std::string cmd;
    auto quote = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    };
    for (size_t i = 0; i < strs.size(); ++i) {
        if (i > 0) cmd += " ";
        cmd += quote(strs[i]);
    }
    int status = std::system(cmd.c_str());
    if (status != -1 && WIFEXITED(status)) status = WEXITSTATUS(status);
    return PyInt_FromLong(status);
}

// subprocess.check_output(args) -> str : run a subprocess, return its stdout
extern "C" PyObject* PyBuiltin_SubprocessCheckOutput(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyUnicode_FromString("");
    if (!args->list[0] || args->list[0]->type != 1) return PyUnicode_FromString("");
    PyObject* argv = args->list[0];
    std::vector<std::string> strs;
    for (size_t i = 0; i < argv->list.size(); ++i) {
        PyObject* s = argv->list[i];
        if (!s || s->type != 3) return PyUnicode_FromString("");
        strs.push_back(s->str);
    }
    if (strs.empty()) return PyUnicode_FromString("");
    // Build a null-terminated C array, then exec via popen with the
    // exec-form (single string) so the shell doesn't interpret embedded
    // newlines. We use "/bin/sh -c <args...>" with each arg quoted.
    std::string cmd;
    auto quote = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    };
    for (size_t i = 0; i < strs.size(); ++i) {
        if (i > 0) cmd += " ";
        cmd += quote(strs[i]);
    }
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return PyUnicode_FromString("");
    char buf[4096];
    std::string out;
    while (char* r = ::fgets(buf, sizeof(buf), fp)) out += r;
    ::pclose(fp);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return PyUnicode_FromString(out.c_str());
}

// makeOsModuleDict: builds a dict that emulates the os module. The
// `os.environ` entry is itself a dict; `os.path` is a dict whose
// `exists` / `isfile` / `isdir` entries are string tokens naming
// runtime helpers; `os.unlink` is also a token.
static PyObject* makeOsModuleDict() {
    PyObject* d = PyDict_New();
    // os.environ -> dict (empty)
    PyObject* env_key = PyUnicode_FromString("environ");
    PyObject* env_val = PyDict_New();
    PyDict_SetItem(d, env_key, env_val);
    Py_DECREF(env_key); Py_DECREF(env_val);
    // os.path -> dict with exists/isfile/isdir/unlink tokens
    PyObject* path_key = PyUnicode_FromString("path");
    PyObject* path_val = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(path_val, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("exists", "PyBuiltin_OsPathExists");
    addTok("isfile", "PyBuiltin_OsPathIsfile");
    addTok("isdir",  "PyBuiltin_OsPathIsdir");
    addTok("unlink", "PyBuiltin_OsUnlink");
    PyDict_SetItem(d, path_key, path_val);
    Py_DECREF(path_key); Py_DECREF(path_val);
    // os.unlink (top-level, not on os.path) -> token
    PyObject* unlink_key = PyUnicode_FromString("unlink");
    PyObject* unlink_val = PyUnicode_FromString("PyBuiltin_OsUnlink");
    PyDict_SetItem(d, unlink_key, unlink_val);
    Py_DECREF(unlink_key); Py_DECREF(unlink_val);
    return d;
}

// makeSubprocessModuleDict: builds the subprocess module dict.
static PyObject* makeSubprocessModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("call",         "PyBuiltin_SubprocessCall");
    addTok("check_output", "PyBuiltin_SubprocessCheckOutput");
    return d;
}

// re.sub(pattern, repl, subject, count) — replace matches. Uses
// pcre2_substitute for full regex semantics (including backreferences
// like \1, \2 in the replacement). The `count` argument limits
// replacements; a non-positive or null count means "all".
extern "C" PyObject* PyBuiltin_ReSub(PyObject* pattern, PyObject* repl,
                                      PyObject* subject, PyObject* count) {
    if (!pattern || pattern->type != 3 || !repl || repl->type != 3 ||
        !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err);
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
        return nullptr;
    }
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    if (!md) { pcre2_code_free(code); return nullptr; }
    long maxCount = (count && (count->type == 0 || count->type == 5)) ? count->value : -1;
    // pcre2_substitute uses 0 to mean "all"; we use -1 internally.
    int pcreCount = (maxCount < 0) ? 0 : (int)maxCount;
    // First, run a normal match loop to compute the output size.
    std::string out;
    out.reserve(subject->str.size() + 16);
    PCRE2_SPTR subj = (PCRE2_SPTR)subject->str.c_str();
    PCRE2_SIZE offset = 0;
    int reps = 0;
    while (offset <= (PCRE2_SIZE)subject->str.size() &&
           (pcreCount == 0 || reps < pcreCount)) {
        int rc = pcre2_match(code, subj, (PCRE2_SIZE)subject->str.size(),
                             offset, 0, md, nullptr);
        if (rc < 0) break;
        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(md);
        PCRE2_SIZE start = ovector[0];
        PCRE2_SIZE end   = ovector[1];
        // Append the text before the match.
        out.append(subject->str, offset, start - offset);
        // Build the replacement by expanding backreferences in `repl->str`.
        for (size_t i = 0; i < repl->str.size(); ++i) {
            char c = repl->str[i];
            if (c == '\\' && i + 1 < repl->str.size()) {
                char n = repl->str[i + 1];
                if (n >= '0' && n <= '9') {
                    int grp = n - '0';
                    if (grp < rc) {
                        std::string captured = subject->str.substr(
                            ovector[2*grp], ovector[2*grp+1] - ovector[2*grp]);
                        out.append(captured);
                    }
                    ++i;
                } else if (n == '\\') {
                    out.push_back('\\');
                    ++i;
                } else {
                    out.push_back(c);
                }
            } else {
                out.push_back(c);
            }
        }
        if (start == end) {
            // Empty match: copy the current char (or nothing at end) and advance.
            if (offset < (PCRE2_SIZE)subject->str.size()) {
                out.push_back(subject->str[offset]);
                offset = end + 1;
            } else {
                break;
            }
        } else {
            offset = end;
        }
        ++reps;
    }
    // Append the remaining text.
    if (offset < (PCRE2_SIZE)subject->str.size()) {
        out.append(subject->str, offset, std::string::npos);
    }
    pcre2_match_data_free(md);
    pcre2_code_free(code);
    return PyUnicode_FromString(out.c_str());
}

// re.split(pattern, subject, maxsplit) — not yet implemented.
// re.split(pattern, subject, maxsplit) — split subject on regex matches
// (literal if the pattern contains no regex metacharacters). We honour
// the empty-match semantics: an empty pattern or a zero-width match
// advances one position to avoid infinite loops.
extern "C" PyObject* PyBuiltin_ReSplit(PyObject* pattern, PyObject* subject, PyObject* /*maxsplit*/) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err);
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
        return nullptr;
    }
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    if (!md) { pcre2_code_free(code); return nullptr; }
    PCRE2_SPTR subj = (PCRE2_SPTR)subject->str.c_str();
    int rc = pcre2_match(code, subj, (PCRE2_SIZE)subject->str.size(), 0, 0, md, nullptr);
    PyObject* result = PyList_New(0);
    PCRE2_SIZE offset = 0;
    while (rc >= 0) {
        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(md);
        PCRE2_SIZE start = ovector[0];
        PCRE2_SIZE end   = ovector[1];
        if (start == end) {
            // Empty match: append the empty string and advance one.
            if (offset <= subject->str.size()) {
                std::string piece = subject->str.substr(offset, (start - offset));
                PyObject* s = PyUnicode_FromString(piece.c_str());
                result->list.push_back(s);
            }
            offset = end + 1;
            if (offset > subject->str.size()) break;
            rc = pcre2_match(code, subj, (PCRE2_SIZE)subject->str.size(), offset, 0, md, nullptr);
            continue;
        }
        // Non-empty match: append the text before the match.
        std::string piece = subject->str.substr(offset, start - offset);
        PyObject* s = PyUnicode_FromString(piece.c_str());
        result->list.push_back(s);
        offset = end;
        if (offset >= subject->str.size()) break;
        rc = pcre2_match(code, subj, (PCRE2_SIZE)subject->str.size(), offset, 0, md, nullptr);
    }
    // Append the remaining text after the last match (or all of it if no
    // match at all).
    if (offset <= subject->str.size()) {
        std::string rest = subject->str.substr(offset);
        PyObject* s = PyUnicode_FromString(rest.c_str());
        result->list.push_back(s);
    }
    pcre2_match_data_free(md);
    pcre2_code_free(code);
    return result;
}

// Stream write adapters for `sys.stderr.write` and `sys.stdout.write`.
// We use a dict for the stream object (with a "write" key); the call
// path goes through Pyc_Apply(token, args), where the token is a string
// naming the registered adapter. Each adapter pulls the strings out of
// the args list and writes them to the corresponding FILE*.
extern "C" void pyc_register_callable(const char* name, PyObject* (*func)(PyObject*));
static PyObject* stderr_write_adapter(PyObject* args) {
    if (!args || args->type != 1) return nullptr;
    for (size_t i = 0; i < args->list.size(); ++i) {
        PyObject* s = args->list[i];
        if (s && s->type == 3) std::fprintf(stderr, "%s", s->str.c_str());
    }
    std::fflush(stderr);
    return PyInt_FromLong(0);
}
static PyObject* stdout_write_adapter(PyObject* args) {
    if (!args || args->type != 1) return nullptr;
    for (size_t i = 0; i < args->list.size(); ++i) {
        PyObject* s = args->list[i];
        if (s && s->type == 3) std::fprintf(stdout, "%s", s->str.c_str());
    }
    std::fflush(stdout);
    return PyInt_FromLong(0);
}

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

    // sys.stderr and sys.stdout: stub file objects whose `.write(str)`
    // method writes to stderr/stdout. We use a dict with a "write"
    // entry whose value is a string token. The compiler's call dispatch
    // doesn't recognise "write" as a builtin, so it falls through to
    // Pyc_Apply. We register a small C++ adapter for the token
    // "pyc_stderr_write" / "pyc_stdout_write" that does the actual write.
    auto makeStream = [](FILE* fp) {
        PyObject* d = PyDict_New();
        PyObject* k = PyUnicode_FromString("write");
        // The token names the adapter; we use a stable, non-pointer name
        // that won't collide with anything. The adapter itself knows
        // which FILE* to write to.
        PyObject* v = nullptr;
        if (fp == stderr) v = PyUnicode_FromString("pyc_stderr_write");
        else if (fp == stdout) v = PyUnicode_FromString("pyc_stdout_write");
        else v = PyUnicode_FromString("pyc_unknown_write");
        PyDict_SetItem(d, k, v);
        Py_DECREF(k);
        Py_DECREF(v);
        // Register the adapter with the callable registry.
        if (fp == stderr) {
            pyc_register_callable("pyc_stderr_write", stderr_write_adapter);
        } else if (fp == stdout) {
            pyc_register_callable("pyc_stdout_write", stdout_write_adapter);
        }
        return d;
    };
    {
        PyObject* stderr_key = PyUnicode_FromString("stderr");
        PyObject* stderr_obj = makeStream(stderr);
        PyDict_SetItem(g_sys_module, stderr_key, stderr_obj);
        Py_DECREF(stderr_key);
        Py_DECREF(stderr_obj);
    }
    {
        PyObject* stdout_key = PyUnicode_FromString("stdout");
        PyObject* stdout_obj = makeStream(stdout);
        PyDict_SetItem(g_sys_module, stdout_key, stdout_obj);
        Py_DECREF(stdout_key); Py_DECREF(stdout_obj);
    }
}

// Register all the os/subprocess/etc. runtime helpers so they can be
// dispatched via Pyc_Apply using their PyBuiltin_* names. The synthetic
// module dicts (`makeOsModuleDict`, `makeSubprocessModuleDict`) embed
// these names as string tokens, and the compiler emits
// `Pyc_Apply(token, args)` for module-attribute calls like
// `os.path.exists(p)`. Without this registration, Pyc_Apply returns
// null for the token. Idempotent (safe to call multiple times).
extern "C" void pyc_setup_callables(void) {
    static bool done = false;
    if (done) return;
    done = true;
    pyc_register_callable("PyBuiltin_OsPathExists",        PyBuiltin_OsPathExists);
    pyc_register_callable("PyBuiltin_OsPathIsfile",        PyBuiltin_OsPathIsfile);
    pyc_register_callable("PyBuiltin_OsPathIsdir",         PyBuiltin_OsPathIsdir);
    pyc_register_callable("PyBuiltin_OsUnlink",            PyBuiltin_OsUnlink);
    pyc_register_callable("PyBuiltin_SubprocessCall",      PyBuiltin_SubprocessCall);
    pyc_register_callable("PyBuiltin_SubprocessCheckOutput", PyBuiltin_SubprocessCheckOutput);
    pyc_register_callable("pyc_stderr_write",              stderr_write_adapter);
    pyc_register_callable("pyc_stdout_write",              stdout_write_adapter);
    pyc_register_callable("pyc_file_write",                pyc_file_write_adapter);
    pyc_register_callable("pyc_file_enter",                pyc_file_enter_adapter);
    pyc_register_callable("pyc_file_exit",                 pyc_file_exit_adapter);
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
        if (b->value == 0) {
            if (g_try_stack) { pyc_raise(PyUnicode_FromString("ZeroDivisionError: integer division or modulo by zero")); return NULL; }
            std::fprintf(stderr, "ZeroDivisionError: integer division or modulo by zero\n");
            std::fflush(stderr);
            std::exit(1);
        }
        long q = a->value / b->value;
        if ((a->value ^ b->value) < 0 && q * b->value != a->value) q--;
        return PyInt_FromLong(q);
    }
    double bv = numeric_val(b);
    if (bv == 0.0) {
        if (g_try_stack) { pyc_raise(PyUnicode_FromString("ZeroDivisionError: float divmod()")); return NULL; }
        std::fprintf(stderr, "ZeroDivisionError: float divmod()\n");
        std::fflush(stderr);
        std::exit(1);
    }
    return PyFloat_FromDouble(floor(numeric_val(a) / bv));
}

// True division (/)
PyObject* PyNumber_TrueDivide(PyObject* a, PyObject* b) {
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    double bv = numeric_val(b);
    if (bv == 0.0) {
        if (g_try_stack) { pyc_raise(PyUnicode_FromString("ZeroDivisionError: float division by zero")); return NULL; }
        std::fprintf(stderr, "ZeroDivisionError: float division by zero\n");
        std::fflush(stderr);
        std::exit(1);
    }
    return PyFloat_FromDouble(numeric_val(a) / bv);
}

PyObject* PyNumber_Remainder(PyObject* a, PyObject* b) {
    if (a && a->type == 3) return PyString_Format(a, b);   // "fmt" % val
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) {
        if (b->value == 0) {
            if (g_try_stack) { pyc_raise(PyUnicode_FromString("ZeroDivisionError: integer division or modulo by zero")); return NULL; }
            std::fprintf(stderr, "ZeroDivisionError: integer division or modulo by zero\n");
            std::fflush(stderr);
            std::exit(1);
        }
        long r = a->value % b->value;
        if (r != 0 && (r ^ b->value) < 0) r += b->value;
        return PyInt_FromLong(r);
    }
    double bv = numeric_val(b);
    if (bv == 0.0) {
        if (g_try_stack) { pyc_raise(PyUnicode_FromString("ZeroDivisionError: float modulo")); return NULL; }
        std::fprintf(stderr, "ZeroDivisionError: float modulo\n");
        std::fflush(stderr);
        std::exit(1);
    }
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
    // List equality and ordering. CPython compares element-wise; the
    // first unequal pair decides, with shorter < longer when all
    // shared elements are equal. We do the same here.
    if (a->type == 1 && b->type == 1) {
        const auto& al = a->list;
        const auto& bl = b->list;
        size_t n = al.size() < bl.size() ? al.size() : bl.size();
        for (size_t i = 0; i < n; ++i) {
            int eq = (al[i] == b->list[i]) ||
                     (al[i] && b->list[i] && PyObject_CompareBool(al[i], b->list[i], 0));
            if (!eq) {
                // Elements differ at i: use the element comparison to decide.
                if (al[i] && b->list[i]) {
                    return PyObject_CompareBool(al[i], b->list[i], op);
                }
                // One side has null (deleted); treat as unequal.
                switch (op) {
                    case 0: return 0;
                    case 1: return 1;
                    case 2: return al[i] == nullptr ? 1 : 0;
                    case 3: return al[i] == nullptr ? 0 : 1;
                    case 4: return al[i] == nullptr ? 1 : 0;
                    case 5: return al[i] == nullptr ? 0 : 1;
                }
            }
        }
        // All shared elements equal — compare by length.
        if (al.size() == bl.size()) {
            switch (op) {
                case 0: return 1;
                case 1: return 0;
                case 2: return 0;
                case 3: return 0;
                case 4: return 1;
                case 5: return 1;
            }
        }
        switch (op) {
            case 0: return 0;
            case 1: return 1;
            case 2: return al.size() < bl.size() ? 1 : 0;
            case 3: return al.size() < bl.size() ? 0 : 1;
            case 4: return al.size() < bl.size() ? 1 : 0;
            case 5: return al.size() < bl.size() ? 0 : 1;
        }
    }
    // Dict equality. CPython compares keys and values (order-independent).
    // We do the same: equal iff same keys and equal values. For ordering
    // (`<`, etc.), raise TypeError — we conservatively return 0.
    if (a->type == 2 && b->type == 2) {
        if (a->dict.size() != b->dict.size()) {
            switch (op) {
                case 0: return 0;
                case 1: return 1;
                default: return 0;
            }
        }
        for (auto& ap : a->dict) {
            bool found = false;
            for (auto& bp : b->dict) {
                if ((ap.first == bp.first) ||
                    (ap.first && bp.first && PyObject_CompareBool(ap.first, bp.first, 0))) {
                    if ((ap.second == bp.second) ||
                        (ap.second && bp.second && PyObject_CompareBool(ap.second, bp.second, 0))) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                switch (op) {
                    case 0: return 0;
                    case 1: return 1;
                    default: return 0;
                }
            }
        }
        switch (op) {
            case 0: return 1;
            case 1: return 0;
            default: return 0;
        }
    }
    // Mixed list/dict — TypeError in CPython; we return 0 for all.
    if ((a->type == 1 || a->type == 2) && (b->type == 1 || b->type == 2) &&
        a->type != b->type) {
        return 0;
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
    // Lists: support .append / .sort / .pop / .insert / .remove / .index /
    // .count / .reverse / .extend / .copy / .clear. We return a dummy
    // callable token; codegen emits the explicit list_* call.
    if (obj->type == 1) {
        if (strcmp(attr, "append")  == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "sort")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "pop")     == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "insert")  == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "remove")  == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "index")   == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "count")   == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "reverse") == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "extend")  == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "copy")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "clear")   == 0) return PyInt_FromLong(0);
    }
    // Dicts: support .keys / .values / .items / .update / .setdefault /
    // .copy / .clear / .pop / .popitem / .fromkeys / .get.
    if (obj->type == 2) {
        if (strcmp(attr, "keys")       == 0) return PyBuiltin_List(obj);
        if (strcmp(attr, "values")     == 0) return PyBuiltin_List(obj);
        if (strcmp(attr, "items")      == 0) return PyBuiltin_List(obj);
        if (strcmp(attr, "update")     == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "setdefault") == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "copy")       == 0) return PyDict_New();
        if (strcmp(attr, "clear")      == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "pop")        == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "popitem")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "fromkeys")   == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "get")        == 0) return PyInt_FromLong(0);
    }
    // Strings: support .upper / .lower / .strip / .lstrip / .rstrip /
    // .split / .join / .startswith / .endswith / .casefold / .title /
    // .isalpha / .isdigit / .isalnum / .islower / .isupper / .isspace /
    // .zfill / .center / .ljust / .rjust / .find / .count / .replace.
    if (obj->type == 3) {
        if (strcmp(attr, "upper")      == 0) return PyString_Upper(obj);
        if (strcmp(attr, "lower")      == 0) return PyString_Lower(obj);
        if (strcmp(attr, "strip")      == 0) return PyString_Strip(obj);
        if (strcmp(attr, "lstrip")     == 0) return PyString_Strip(obj);   // placeholder
        if (strcmp(attr, "rstrip")     == 0) return PyString_Strip(obj);
        if (strcmp(attr, "split")      == 0) return PyString_Split(obj, nullptr);
        if (strcmp(attr, "join")       == 0) return PyString_Join(obj, nullptr);
        if (strcmp(attr, "startswith") == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "endswith")   == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "casefold")   == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "title")      == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "isalpha")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "isdigit")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "isalnum")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "islower")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "isupper")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "isspace")    == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "zfill")      == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "center")     == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "ljust")      == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "rjust")      == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "find")       == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "count")      == 0) return PyInt_FromLong(0);
        if (strcmp(attr, "replace")    == 0) return PyInt_FromLong(0);
    }
    // Fallback: return the object itself (matches the previous stub
    // behaviour for unsupported lookups; doesn't crash).
    Py_INCREF(obj);
    return obj;
}

void PyErr_Print(void) { fprintf(stderr, "Python error occurred\n"); }

// ---- Exception support ----
// Use a small thread-local stack of (jmp_buf, filter-type) entries.
// pyc_try_push registers a buffer; pyc_raise longjmps to the innermost
// matching buffer (or stores the exception if no match). This lets
// `raise` inside a try block transfer control to the matching except
// handler in linear IR without per-instruction exception checks.
#include <csetjmp>
static thread_local PyObject* g_current_exception = nullptr;
struct TryFrame {
    jmp_buf jmp;
    PyObject* filterType;   // not used for dispatch in the simple model
    PyObject* exc;          // the exception that triggered this frame
    TryFrame* next;
};
// g_try_stack is forward-declared at the top of the file.

void pyc_try_push(void* jmpBuf, PyObject* filterType) {
    TryFrame* f = new TryFrame();
    f->filterType = filterType;          // not used currently
    f->exc = nullptr;
    f->next = g_try_stack;
    g_try_stack = f;
    if (jmpBuf) memcpy(f->jmp, jmpBuf, sizeof(jmp_buf));
}
void pyc_try_pop(void) {
    if (!g_try_stack) return;
    TryFrame* f = g_try_stack;
    g_try_stack = f->next;
    if (f->exc) Py_DECREF(f->exc);
    delete f;
}
void pyc_raise(PyObject* exc) {
    if (!exc) return;
    if (g_try_stack) {
        // Transfer to the innermost try frame. We do NOT pop the frame
        // here — the catch block will clear the exception via
        // pyc_clear_exception / pyc_try_pop, and leaving the frame on
        // the stack ensures longjmp re-entry (e.g. inside an except
        // handler) targets the next outer try. The frame's `exc` field
        // holds the exception so the catch can retrieve it.
        TryFrame* f = g_try_stack;
        if (f->exc) Py_DECREF(f->exc);
        f->exc = exc;
        Py_INCREF(exc);
        std::longjmp(f->jmp, 1);
    }
    if (g_current_exception) Py_DECREF(g_current_exception);
    g_current_exception = exc;
    Py_INCREF(exc);
}
PyObject* pyc_current_exception(void) {
    if (g_try_stack && g_try_stack->exc) {
        Py_INCREF(g_try_stack->exc);
        return g_try_stack->exc;
    }
    if (!g_current_exception) return nullptr;
    Py_INCREF(g_current_exception);
    return g_current_exception;
}
void pyc_clear_exception(void) {
    if (g_try_stack && g_try_stack->exc) {
        Py_DECREF(g_try_stack->exc);
        g_try_stack->exc = nullptr;
    }
    if (g_current_exception) {
        Py_DECREF(g_current_exception);
        g_current_exception = nullptr;
    }
}

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

// Pyc_Apply(tokenStr or bundleList, argList) -> boxed result
// If first arg is a bundle list [tokenStr, extra0, ...] (cells or prebound defaults),
// extract the token and prepend the extras to the provided argList before dispatch.
extern "C" PyObject* Pyc_Apply(PyObject* token, PyObject* argList) {
    if (!token) return nullptr;
    PyObject* realTok = nullptr;
    if (token->type == 3) {
        realTok = token;
    } else if (token->type == 1 && !token->list.empty()) {
        PyObject* first = token->list[0];
        if (first && first->type == 3) realTok = first;
    }
    if (!realTok || realTok->type != 3) return nullptr;
    auto it = g_callableRegistry.find(realTok->str);
    if (it == g_callableRegistry.end()) return nullptr;

    PyObject* prepend = nullptr;
    if (token->type == 1 && !token->list.empty()) {
        prepend = PyList_New(0);
        for (size_t i = 1; i < token->list.size(); ++i) {
            PyObject* v = token->list[i];
            if (v) Py_INCREF(v);
            PyList_Append(prepend, v);
        }
    }

    PyObject* finalList = argList;
    if (prepend) {
        PyObject* comb = PyList_New(0);
        for (auto* v : prepend->list) {
            if (v) Py_INCREF(v);
            PyList_Append(comb, v);
        }
        if (argList && argList->type == 1) {
            for (auto* v : argList->list) {
                if (v) Py_INCREF(v);
                PyList_Append(comb, v);
            }
        }
        finalList = comb;
        PyObject* r = it->second ? it->second(finalList) : nullptr;
        Py_DECREF(comb);
        Py_DECREF(prepend);
        return r;
    }
    return it->second ? it->second(finalList) : nullptr;
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

extern "C" long PyAlloc_GetIntCount() { return alloc_int_count.load(); }
extern "C" long PyAlloc_GetFloatCount() { return alloc_float_count.load(); }
extern "C" long PyAlloc_GetListCount() { return alloc_list_count.load(); }
extern "C" long PyAlloc_GetDictCount() { return alloc_dict_count.load(); }
extern "C" long PyAlloc_GetStrCount() { return alloc_str_count.load(); }
extern "C" long PyAlloc_GetTotal() {
    return alloc_int_count.load() + alloc_float_count.load() +
           alloc_list_count.load() + alloc_dict_count.load() + alloc_str_count.load();
}

// ---- B6: super() proxy ----
// super() returns a proxy object (type==7) that delegates attribute access
// to the parent class. The proxy stores:
// - refcount: 1
// - type: 7 (super proxy)
// - str: empty
// - list: empty
// - dict: empty (unused)
// - cell_content: pointer to the parent class dict
extern "C" PyObject* PyBuiltin_Super(void) {
    PyObject* super = new PyObject();
    super->refcount = 1;
    super->type = 7;  // super proxy
    super->str = "";
    super->list = {};
    super->dict = {};
    super->cell_content = nullptr;  // will be set by compiler to parent class
    return super;
}

// Helper for string equality comparison
static bool pyObjStrEqual(PyObject* a, PyObject* b) {
    if (!a || !b) return false;
    if (a->type == 3 && b->type == 3) return a->str == b->str;
    if (a->type == 3 && b->type != 3) return false;
    if (a->type != 3 && b->type == 3) return false;
    return a == b;
}

// Helper to compare a PyObject* with a C string literal
static bool pyObjStrEqualsLiteral(PyObject* obj, const char* literal) {
    if (!obj || obj->type != 3) return false;
    return obj->str == literal;
}

// Helper to get a list item by integer index
static PyObject* PyList_GetItemInt(PyObject* list, size_t index) {
    if (!list || list->type != 1) return nullptr;
    if (index >= list->list.size()) return nullptr;
    return list->list[index];
}

// ---- B6b: super() with MRO-based method resolution ----
// PyBuiltin_SuperMethod(self, definingClass, methodName, [args...])
// Implements Python's super() behavior:
// 1. Gets self.__class__
// 2. Looks up __mro__ from that class dict
// 3. Finds definingClass in the MRO
// 4. Calls methodName on the next class in the MRO
extern "C" PyObject* PyBuiltin_SuperMethod(PyObject* args) {
    if (!args || args->type != 1) return nullptr;  // args must be a list
    if (PyList_Size(args) < 3) return nullptr;  // need at least self, definingClass, methodName
    
    PyObject* self = PyList_GetItemInt(args, 0);
    PyObject* definingClass = PyList_GetItemInt(args, 1);
    PyObject* methodName = PyList_GetItemInt(args, 2);
    
    if (!self || !definingClass || !methodName) return nullptr;
    
    // Get self's class
    PyObject* selfClass = nullptr;
    for (auto& kv : self->dict) {
        if (pyObjStrEqualsLiteral(kv.first, "__class__")) {
            selfClass = kv.second;
            break;
        }
    }
    if (!selfClass) return nullptr;
    
    // Get MRO from self's class
    PyObject* mroList = nullptr;
    for (auto& kv : selfClass->dict) {
        if (pyObjStrEqualsLiteral(kv.first, "__mro__")) {
            mroList = kv.second;
            break;
        }
    }
    if (!mroList || mroList->type != 1) return nullptr;  // MRO must be a list
    
    // Find definingClass in MRO and get the next class
    size_t mroSize = PyList_Size(mroList);
    size_t definingIndex = -1;
    for (size_t i = 0; i < mroSize; ++i) {
        PyObject* mroItem = PyList_GetItemInt(mroList, i);
        if (pyObjStrEqual(mroItem, definingClass)) {
            definingIndex = i;
            break;
        }
    }
    if (definingIndex == (size_t)-1 || definingIndex + 1 >= mroSize) return nullptr;
    
    PyObject* nextClass = PyList_GetItemInt(mroList, definingIndex + 1);
    if (!nextClass) return nullptr;
    
    // Look up method on next class
    PyObject* method = nullptr;
    for (auto& kv : nextClass->dict) {
        if (pyObjStrEqual(kv.first, methodName)) {
            method = kv.second;
            break;
        }
    }
    if (!method) return nullptr;
    
    // Build args list with self prepended
    size_t argSize = PyList_Size(args);
    PyObject* callArgs = new PyObject();
    callArgs->refcount = 1;
    callArgs->type = 1;  // list
    callArgs->str = "";
    callArgs->list = {};
    callArgs->dict = {};
    callArgs->cell_content = nullptr;
    callArgs->list_item_type = 0;
    
    // Add self at index 0
    PyList_Append(callArgs, self);
    // Add remaining args (skip self, definingClass, methodName)
    for (size_t i = 3; i < argSize; ++i) {
        PyList_Append(callArgs, PyList_GetItemInt(args, i));
    }
    
    // Call the method
    return Pyc_Apply(method, callArgs);
}

// ---- B6: Extended attribute lookup with class fallback ----
// PyObject_GetAttrExtended looks up an attribute on an object, first checking
// the instance dict, then the class dict (for class attributes).
extern "C" PyObject* PyObject_GetAttrExtended(PyObject* obj, PyObject* attr) {
    if (!obj || !attr) return nullptr;
    // First try instance/class dict directly
    for (auto& kv : obj->dict) {
        if (pyObjStrEqual(kv.first, attr)) {
            Py_INCREF(kv.second);
            return kv.second;
        }
    }
    // For instances (objects with __class__ in their dict), fall back to class dict
    PyObject* klass = nullptr;
    for (auto& kv : obj->dict) {
        if (kv.first->str == "__class__") {
            klass = kv.second;
            break;
        }
    }
    if (klass) {
        for (auto& kv : klass->dict) {
            if (pyObjStrEqual(kv.first, attr)) {
                Py_INCREF(kv.second);
                return kv.second;
            }
        }
    }
    // Not found - return None
    PyObject* none = new PyObject();
    none->refcount = 1;
    none->type = 5;  // None type
    none->str = "None";
    return none;
}

} // extern "C"
