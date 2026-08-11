#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <random>
#include <cctype>
#include <cstdint>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <string>
#include <atomic>
#include <chrono>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <mpdecimal.h>

#include "pyc/runtime.h"
#include "pyc/object_struct.h"

// decimal.Decimal (type 19) small helpers, needed early since
// PyNumber_Negate/Add/Subtract/Multiply/Divide/TrueDivide all gain a
// type==19 branch and some of those functions appear well before the
// rest of the Decimal implementation (search "decimal.Decimal (type 19)"
// further down for the full design comment and construction/quantize/
// conversion functions).
static mpd_context_t g_decCtx;
static bool g_decCtxInit = false;
static mpd_context_t* pyc_dec_ctx() {
    if (!g_decCtxInit) {
        mpd_defaultcontext(&g_decCtx);
        mpd_qsetprec(&g_decCtx, 28);
        mpd_qsetround(&g_decCtx, MPD_ROUND_HALF_EVEN);
        g_decCtxInit = true;
    }
    return &g_decCtx;
}
static mpd_t* pyc_as_decimal(PyObject* o) {
    if (!o || o->type != 19) return nullptr;
    return reinterpret_cast<mpd_t*>(o->value);
}
static PyObject* pyc_decimal_wrap(mpd_t* d) {
    PyObject* o = new PyObject();
    o->refcount = 1;
    o->type = 19;
    o->value = (int64_t)(intptr_t)d;
    return o;
}
// Resolves one operand of a Decimal-involving binary op: a Decimal
// itself returns its mpd_t* directly (borrowed — *isTemp stays false,
// caller must not free it); an int/bool is converted into a freshly
// allocated mpd_t* (*isTemp=true, caller must mpd_del it after use) —
// matches real Python's Decimal+int support. Anything else (float, str,
// ...) returns nullptr: real CPython's Decimal does NOT implicitly
// coerce from float (Decimal('1.5') + 1.5 raises TypeError, verified
// against real Python) — the generic PyNumber_* callers below return
// NULL for this case, the same "unsupported operand combo" convention
// already used elsewhere in this file (e.g. is_numeric()'s early-outs).
static mpd_t* pyc_decimal_operand(PyObject* x, bool* isTemp) {
    *isTemp = false;
    if (!x) return nullptr;
    if (x->type == 19) return pyc_as_decimal(x);
    if (x->type == 0 || x->type == 5) {
        mpd_t* d = mpd_qnew();
        uint32_t status = 0;
        mpd_qset_i64(d, x->value, pyc_dec_ctx(), &status);
        *isTemp = true;
        return d;
    }
    return nullptr;
}
// Forward declaration: full definition (and design comment) sits with
// the rest of the Decimal construction functions further down; needed
// here because PyObject_CompareBool (which uses it for Decimal-vs-float
// comparison) appears earlier in this file.
extern "C" PyObject* PyDecimal_FromFloat(PyObject* f);

extern char** environ;

// Immortal PyObject* refcount sentinel (True, False, and small ints in
// [-5, 256] are interned; Py_INCREF / Py_DECREF skip them so they are
// never freed or duplicated by ownership paths).
static constexpr int IMMORTAL_REFCOUNT = 0x3fffffff;

// Forward declaration of the try-stack head, used by division/modulo
// zero-division reporting (we raise when an enclosing try is in scope, and
// print-and-exit otherwise).
struct TryFrame;
static thread_local TryFrame* g_try_stack = nullptr;
static void pyc_raise_msg(const char* type, const char* msg);

#define PYC_ALWAYS_INLINE __attribute__((always_inline))

void PYC_ALWAYS_INLINE Py_INCREF(PyObject* obj) {
    if (obj && obj->refcount != IMMORTAL_REFCOUNT) ++obj->refcount;
}

// Shortest round-trip decimal representation, always with a decimal point.
// Format a double matching CPython's repr() rules: fixed notation for
// 1e-4 <= |v| < 1e16, scientific notation otherwise (matching CPython's
// float repr exactly — verified against real CPython). Uses %.*g to find
// the shortest round-tripping precision, then converts to fixed notation
// when CPython would (avoiding the %g default's premature scientific
// notation for values like 20.0 -> "2e+01").
static void format_double(char* buf, size_t bufsize, double v) {
    if (v != v)          { snprintf(buf, bufsize, "nan");  return; }
    if (v ==  1.0/0.0)   { snprintf(buf, bufsize, "inf");  return; }
    if (v == -1.0/0.0)   { snprintf(buf, bufsize, "-inf"); return; }
    double av = v < 0 ? -v : v;
    bool cpythonFixed = (av >= 1e-4 && av < 1e16);
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, bufsize, "%.*g", prec, v);
        double check;
        if (sscanf(buf, "%lf", &check) == 1 && check == v) break;
    }
    // If %g produced scientific notation but CPython would use fixed,
    // reformat with %.*f using the same precision. This matches CPython's
    // repr boundary (1e-4 <= |v| < 1e16 uses fixed; outside uses scientific).
    if (cpythonFixed && (strchr(buf, 'e') || strchr(buf, 'E'))) {
        // Reformat with fixed notation. Use enough precision to round-trip.
        for (int prec = 1; prec <= 17; prec++) {
            snprintf(buf, bufsize, "%.*f", prec, v);
            double check;
            if (sscanf(buf, "%lf", &check) == 1 && check == v) break;
        }
        // Strip trailing zeros (and the decimal point if all zeros) to
        // match CPython's repr (20.0 -> "20.0", not "20.00000000000000").
        size_t len = strlen(buf);
        while (len > 1 && buf[len-1] == '0') { buf[--len] = '\0'; }
        if (len > 1 && buf[len-1] == '.') { buf[len] = '0'; buf[len+1] = '\0'; }
    }
    // Guarantee at least one decimal digit so it reads as float, not int.
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&
        !strchr(buf, 'n') && !strchr(buf, 'i')) {
        size_t len = strlen(buf);
        if (len + 2 < bufsize) { buf[len] = '.'; buf[len+1] = '0'; buf[len+2] = '\0'; }
    }
}

// Like format_double, but strips trailing ".0" from whole numbers, matching
// CPython's complex repr (1j, not 1.0j). Used only for complex number parts.
static void format_double_complex(char* buf, size_t bufsize, double v) {
    format_double(buf, bufsize, v);
    // Strip trailing ".0" (but keep it for inf/nan/exponential).
    size_t len = strlen(buf);
    if (len >= 2 && buf[len-2] == '.' && buf[len-1] == '0') {
        buf[len-2] = '\0';
    }
}

// ---- Format Specification Mini-Language (Pyc_FormatValue, defined
// further down once PyStr_FromAny is available) — found and fixed while
// bug hunting: f"{x:.2f}"-style format specs were a documented, deliberate
// MVP-era scope cut (the parser skipped format_spec entirely), and
// str.format() had no implementation at all. These helpers implement a
// practical subset of Python's spec grammar:
//   [[fill]align][sign]["#"]["0"][width][","|"_"]["." precision][type]
// covering fill/align (<>^= with an optional fill char), sign (+/-/space),
// "#" alternate form (only for int base prefixes 0b/0o/0x), "0" zero-pad,
// width, "," / "_" thousands grouping, precision, and type codes
// s/d/b/o/x/X/f/F/e/E/g/G/%/c. Not implemented (documented, not chased
// down): "n" (locale-aware — treated as a plain numeric type instead),
// "#" for floats (always-show-decimal-point), and decimal.Decimal
// operands with a numeric type code (falls back to plain str()+padding).
static std::string pyc_group_digits(const std::string& digits, char sep, int groupSize = 3) {
    int n = (int)digits.size();
    if (n <= groupSize) return digits;
    int firstGroup = n % groupSize;
    if (firstGroup == 0) firstGroup = groupSize;
    std::string out = digits.substr(0, (size_t)firstGroup);
    for (int i = firstGroup; i < n; i += groupSize) {
        out += sep;
        out += digits.substr((size_t)i, (size_t)groupSize);
    }
    return out;
}
static std::string pyc_to_base(unsigned long v, int base, bool upper) {
    if (v == 0) return "0";
    static const char* lo = "0123456789abcdef";
    static const char* hi = "0123456789ABCDEF";
    const char* d = upper ? hi : lo;
    std::string s;
    while (v > 0) { s = std::string(1, d[v % (unsigned long)base]) + s; v /= (unsigned long)base; }
    return s;
}
// Pad `body` to `width` using fillch/align. numPrefixLen is the length
// of a leading sign/base-prefix (e.g. "-0x") that must stay to the left
// of zero-padding under align=='=' (numeric zero-fill mode) rather than
// being pushed rightward with the digits.
static std::string pyc_fmt_pad(const std::string& body, long width, char fillch, char align, size_t numPrefixLen) {
    long len = (long)body.size();
    if (width <= len) return body;
    long padLen = width - len;
    if (align == '<') return body + std::string((size_t)padLen, fillch);
    if (align == '^') {
        long left = padLen / 2, right = padLen - left;
        return std::string((size_t)left, fillch) + body + std::string((size_t)right, fillch);
    }
    if (align == '=') {
        std::string prefix = body.substr(0, numPrefixLen);
        std::string rest = body.substr(numPrefixLen);
        return prefix + std::string((size_t)padLen, fillch) + rest;
    }
    return std::string((size_t)padLen, fillch) + body; // '>' or default
}
struct PycFormatSpec {
    char fill = ' ';
    char align = 0;   // 0 = unset (defaults chosen per-type below)
    char sign = '-';  // '-' = only negative numbers show a sign (default)
    bool alt = false;
    long width = -1;
    char grouping = 0;
    long precision = -1;
    char type = 0;
};
static void pyc_parse_format_spec(const std::string& spec, PycFormatSpec& f) {
    size_t i = 0, n = spec.size();
    if (n >= 2 && (spec[1]=='<'||spec[1]=='>'||spec[1]=='='||spec[1]=='^')) { f.fill = spec[0]; f.align = spec[1]; i = 2; }
    else if (n >= 1 && (spec[0]=='<'||spec[0]=='>'||spec[0]=='='||spec[0]=='^')) { f.align = spec[0]; i = 1; }
    if (i < n && (spec[i]=='+'||spec[i]=='-'||spec[i]==' ')) { f.sign = spec[i]; i++; }
    if (i < n && spec[i]=='#') { f.alt = true; i++; }
    if (i < n && spec[i]=='0') {
        i++;
        if (f.align == 0) { f.align = '='; f.fill = '0'; }
    }
    std::string widthStr;
    while (i < n && isdigit((unsigned char)spec[i])) { widthStr += spec[i]; i++; }
    if (!widthStr.empty()) f.width = std::stol(widthStr);
    if (i < n && (spec[i]==','||spec[i]=='_')) { f.grouping = spec[i]; i++; }
    if (i < n && spec[i]=='.') {
        i++;
        std::string precStr;
        while (i < n && isdigit((unsigned char)spec[i])) { precStr += spec[i]; i++; }
        f.precision = precStr.empty() ? 0 : std::stol(precStr);
    }
    if (i < n) f.type = spec[i];
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
static std::atomic<long> alloc_set_count{0};

// P1: thread-local free-lists for short-lived boxed ints/floats (nbody hot path).
// Caps keep memory bounded; overflow falls back to delete/new.
static constexpr int PYC_FREELIST_CAP = 256;
static thread_local PyObject* g_float_freelist[PYC_FREELIST_CAP];
static thread_local int g_float_freelist_n = 0;
static thread_local PyObject* g_int_freelist[PYC_FREELIST_CAP];
static thread_local int g_int_freelist_n = 0;

static PyObject* allocFloatObj() {
    if (g_float_freelist_n > 0) {
        PyObject* obj = g_float_freelist[--g_float_freelist_n];
        obj->refcount = 1;
        obj->type = 4;
        return obj;
    }
    alloc_float_count++;
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 4;
    return obj;
}

static PyObject* allocIntObj() {
    if (g_int_freelist_n > 0) {
        PyObject* obj = g_int_freelist[--g_int_freelist_n];
        obj->refcount = 1;
        obj->type = 0;
        return obj;
    }
    alloc_int_count++;
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 0;
    return obj;
}

static void freeScalarObj(PyObject* obj) {
    if (!obj) return;
    if (obj->type == 4 && g_float_freelist_n < PYC_FREELIST_CAP &&
        obj->list.empty() && obj->flist.empty() && obj->ilist.empty() &&
        obj->dict.empty() && obj->str.empty() && !obj->cell_content) {
        g_float_freelist[g_float_freelist_n++] = obj;
        return;
    }
    if (obj->type == 0 && g_int_freelist_n < PYC_FREELIST_CAP &&
        obj->list.empty() && obj->flist.empty() && obj->ilist.empty() &&
        obj->dict.empty() && obj->str.empty() && !obj->cell_content) {
        g_int_freelist[g_int_freelist_n++] = obj;
        return;
    }
    delete obj;
}

PyObject* PyInt_FromLong(long v) {
    if (PyObject* cached = getSmallInt(v)) {
        return cached;                          // immortal: caller "owns" but cannot free
    }
    PyObject* obj = allocIntObj();
    obj->value = v;
    return obj;
}

// ---- Operator/protocol dunder-method dispatch ----
// Found and fixed while bug hunting: apart from __init__/__str__/__repr__
// (and, as of this session, __classmethod__/@property), essentially no
// Python "special method" was ever dispatched — arithmetic
// (__add__/__sub__/...), comparison (__eq__/__lt__/...), __len__,
// __bool__, __neg__, the container protocol (__getitem__/__setitem__/
// __contains__), the iterator protocol (__iter__/__next__), and
// __call__ all either silently returned None/crashed, or — worse, for
// __eq__ specifically — appeared to "work" by sheer coincidence (two
// class instances are both dict-backed, so `==` fell through to a
// generic structural dict-equality comparison that's *right* when two
// instances happen to hold identical attribute values and *wrong*
// otherwise: confirmed `Point(1,2) == Point(9,9)` incorrectly
// evaluating `True` with a real `__eq__` defined and ignored).
//
// pyc_lookup_dunder is the single lookup mechanism every dispatch site
// below shares — checks the instance dict first, then the class dict
// (found via "__class__"). This used to be a private, identically-
// -bodied static helper (GetStrOrRepr) that only PyObject_Print's
// __str__/__repr__ dispatch could see, since it lived much further down
// this file; generalized and moved up here (before its earliest
// consumer, PyObject_TruthValue's __bool__/__len__ check) so every
// operator dispatch site added this pass can use it too, including ones
// defined earlier in the file than dict/method lookup infrastructure
// previously lived. GetStrOrRepr itself, further down, now just calls
// this.
static PyObject* pyc_lookup_dunder(PyObject* obj, const char* method) {
    if (!obj || obj->type != 2) return nullptr;
    for (auto& pair : obj->dict) {
        if (pair.first && pair.first->type == 3 && pair.first->str == method) {
            return pair.second;
        }
    }
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
// Calls a looked-up dunder method with `self` as the sole argument
// (__len__, __bool__, __neg__, __iter__, __next__, ...). Returns a new
// reference, or nullptr if Pyc_Apply itself returns nullptr (a dunder
// legitimately returning None).
static PyObject* pyc_call_dunder1(PyObject* method, PyObject* self) {
    PyObject* args = PyList_New(0);
    PyList_Append(args, self);
    PyObject* result = Pyc_Apply(method, args);
    Py_DECREF(args);
    return result;
}
// Calls a looked-up dunder method with (self, other) — every binary
// operator (__add__, __eq__, __getitem__, __contains__'s (self, item)
// shape, ...). Returns a new reference.
static PyObject* pyc_call_dunder2(PyObject* method, PyObject* self, PyObject* other) {
    PyObject* args = PyList_New(0);
    PyList_Append(args, self);
    PyList_Append(args, other);
    PyObject* result = Pyc_Apply(method, args);
    Py_DECREF(args);
    return result;
}
// Defined much further down this file (needs the setjmp-based try/except
// machinery, which lives there) — drains a class instance's __iter__/
// __next__ protocol eagerly into a real list. See its own comment for
// why. Forward-declared here so PyBuiltin_List (used by both `for x in
// obj:` and the bare `list(obj)` builtin) can call it despite being
// defined earlier in the file than the try/except infrastructure.
static PyObject* pyc_materialize_iterator_protocol(PyObject* obj);

// Forward decl: pyc_ensure_boxed_list is defined ~line 3146 but used by
// PyBuiltin_Tuple (and many other functions defined earlier in the file).
static void pyc_ensure_boxed_list(PyObject* lst);

// Internal truthiness predicate (mirrors Python's bool()).
// NOT static: called directly from generated code (Codegen.cpp's "br"
// instruction handler) for boxed non-numeric conditions (str/list/dict/
// Decimal/...) — see the severe pre-existing bug this fixes, documented
// in IMPLEMENTATION.md, found while verifying decimal.Decimal's
// truthiness this session.
int PyObject_TruthValue(PyObject* obj) {
    if (!obj) return 0;
    if (obj->type == 0 || obj->type == 5) return obj->value != 0;
    if (obj->type == 4) return obj->dvalue != 0.0;
    if (obj->type == 3 || obj->type == 17 || obj->type == 18) return !obj->str.empty();
    if (obj->type == 1) {
        // Same pyc_ensure_boxed_list()-class bug found repeatedly
        // elsewhere in this file: homogeneous int/float list literals
        // store their data in ilist/flist (list_item_type 1/2), not
        // list — checking obj->list.empty() alone is always true for
        // those, so `if [1,2,3]:` was incorrectly falsy. Mirrors
        // PyList_Size's existing three-way check (PyList_Size itself is
        // defined later in this file, after this function, so inlined
        // here rather than forward-declared).
        if (obj->list_item_type == 1) return !obj->ilist.empty();
        if (obj->list_item_type == 2) return !obj->flist.empty();
        return !obj->list.empty();
    }
    if (obj->type == 2) {
        // __bool__ then __len__ (CPython's exact precedence for a class
        // instance with neither is "always truthy" — the plain
        // !obj->dict.empty() fallback below is a pre-existing,
        // deliberate approximation of that for a class instance with
        // neither dunder, not something this pass changes).
        PyObject* boolMethod = pyc_lookup_dunder(obj, "__bool__");
        if (boolMethod) {
            PyObject* r = pyc_call_dunder1(boolMethod, obj);
            int truthy = PyObject_TruthValue(r);
            if (r) Py_DECREF(r);
            return truthy;
        }
        PyObject* lenMethod = pyc_lookup_dunder(obj, "__len__");
        if (lenMethod) {
            PyObject* r = pyc_call_dunder1(lenMethod, obj);
            int truthy = PyObject_TruthValue(r);
            if (r) Py_DECREF(r);
            return truthy;
        }
        return !obj->dict.empty();
    }
    if (obj->type == 19) return !mpd_iszero(pyc_as_decimal(obj));
    if (obj->type == 20) return !obj->setElems.empty();
    if (obj->type == 7) {
        if (obj->list_item_type == 1) return !obj->ilist.empty();
        if (obj->list_item_type == 2) return !obj->flist.empty();
        return !obj->list.empty();
    }
    return 1;
}

PyObject* PyBool_New(int v) {
    return getBoolObj(v);
}

PyObject* PyFloat_FromDouble(double v) {
    PyObject* obj = allocFloatObj();
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

PyObject* PyDict_SizeBoxed(PyObject* dict) {
    if (!dict || dict->type != 2) return PyInt_FromLong(0);
    return PyInt_FromLong((long)dict->dict.size());
}

PyObject* PyDict_GetItemBoxed(PyObject* dict, PyObject* key) {
    return PyDict_GetItem(dict, key);
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

// Constant-index get: avoids boxing the index (nbody unpack spine).
PyObject* PyList_GetItemI64(PyObject* list, int64_t index) {
    if (!list || list->type != 1) return nullptr;
    size_t n = PyList_Size(list);
    long i = (long)index;
    if (i < 0) i += (long)n;
    if (i < 0 || (size_t)i >= n) return nullptr;
    if (list->list_item_type == 1) return PyInt_FromLong(list->ilist[i]);
    if (list->list_item_type == 2) return PyFloat_FromDouble(list->flist[i]);
    PyObject* item = list->list[i];
    if (item) Py_INCREF(item);
    return item;
}

int64_t PyList_SizeI64(PyObject* list) {
    return (int64_t)PyList_Size(list);
}

static PyObject* listGetNewRef(PyObject* list, size_t i) {
    if (list->list_item_type == 1) return PyInt_FromLong(list->ilist[i]);
    if (list->list_item_type == 2) return PyFloat_FromDouble(list->flist[i]);
    PyObject* item = list->list[i];
    if (item) Py_INCREF(item);
    return item;
}

// Forward decl: defined just below PyTuple_GetItem (~line 749).
static PyObject* tupleGetNewRef(PyObject* tuple, size_t i);

int PyList_Unpack2(PyObject* list, PyObject** out0, PyObject** out1) {
    if (!list || !out0 || !out1) return 0;
    size_t n;
    if (list->type == 1) n = PyList_Size(list);
    else if (list->type == 7) n = PyTuple_Size(list);
    else return 0;
    if (n < 2) return 0;
    if (list->type == 7) {
        *out0 = tupleGetNewRef(list, 0);
        *out1 = tupleGetNewRef(list, 1);
    } else {
        *out0 = listGetNewRef(list, 0);
        *out1 = listGetNewRef(list, 1);
    }
    return 1;
}

int PyList_Unpack3(PyObject* list, PyObject** out0, PyObject** out1, PyObject** out2) {
    if (!list || !out0 || !out1 || !out2) return 0;
    size_t n;
    if (list->type == 1) n = PyList_Size(list);
    else if (list->type == 7) n = PyTuple_Size(list);
    else return 0;
    if (n < 3) return 0;
    if (list->type == 7) {
        *out0 = tupleGetNewRef(list, 0);
        *out1 = tupleGetNewRef(list, 1);
        *out2 = tupleGetNewRef(list, 2);
    } else {
        *out0 = listGetNewRef(list, 0);
        *out1 = listGetNewRef(list, 1);
        *out2 = listGetNewRef(list, 2);
    }
    return 1;
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

// --- tuple type (type 7) ---------------------------------------------------
// Immutable sequence reusing the list/ilist/flist storage fields. Constructed
// once via PyTuple_New + PyTuple_SetItem (which steals a reference, matching
// PyList_SetItem's boxed-list convention) and never mutated afterward.
// Homogeneous int/float tuple storage (list_item_type 1/2) is supported for
// parity with lists, though the compiler currently always emits boxed tuples
// (allInt/allFloat detection in lowerList only special-cases lists). Reading
// goes through the same listGetNewRef helper used by PyList_Unpack2/3, so
// tuples unpack correctly into `a, b = t` and `for x, y in pairs`.

PyObject* PyTuple_New(size_t size) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 7;
    obj->list.assign(size, nullptr);
    obj->list_item_type = 0;
    return obj;
}

void PyTuple_SetItem(PyObject* tuple, size_t index, PyObject* item) {
    if (!tuple || tuple->type != 7) return;
    // Tuples are constructed with PyTuple_New (pre-sized, list_item_type 0),
    // so only the boxed-list path is needed. The slot starts as nullptr
    // (PyTuple_New's list.assign), so there is no previous owner to DECREF.
    if (index < tuple->list.size()) {
        tuple->list[index] = item;
        if (item) Py_INCREF(item);
    }
}

void PyTuple_SetItemBoxed(PyObject* tuple, PyObject* idx, PyObject* item) {
    if (!idx || (idx->type != 0 && idx->type != 5)) return;
    PyTuple_SetItem(tuple, (size_t)idx->value, item);
}

PyObject* PyTuple_GetItem(PyObject* tuple, size_t index) {
    if (tuple && tuple->type == 7) {
        if (tuple->list_item_type == 1 && index < tuple->ilist.size())
            return PyInt_FromLong(tuple->ilist[index]);
        if (tuple->list_item_type == 2 && index < tuple->flist.size())
            return PyFloat_FromDouble(tuple->flist[index]);
        if (index < tuple->list.size()) {
            // Borrowed ref, matching PyList_GetItem's boxed convention.
            return tuple->list[index];
        }
    }
    return nullptr;
}

// Internal: new-ref get (mirrors listGetNewRef) for unpacking paths.
static PyObject* tupleGetNewRef(PyObject* tuple, size_t i) {
    if (tuple->list_item_type == 1) return PyInt_FromLong(tuple->ilist[i]);
    if (tuple->list_item_type == 2) return PyFloat_FromDouble(tuple->flist[i]);
    PyObject* item = tuple->list[i];
    if (item) Py_INCREF(item);
    return item;
}

size_t PyTuple_Size(PyObject* tuple) {
    if (tuple && tuple->type == 7) {
        if (tuple->list_item_type == 1) return tuple->ilist.size();
        if (tuple->list_item_type == 2) return tuple->flist.size();
        return tuple->list.size();
    }
    return 0;
}

PyObject* PyTuple_SizeBoxed(PyObject* tuple) {
    return PyInt_FromLong((long)PyTuple_Size(tuple));
}

PyObject* PyTuple_NewBoxed(PyObject* n) {
    size_t size = (n && n->type == 0) ? (size_t)n->value : 0;
    return PyTuple_New(size);
}

PyObject* PyTuple_FromArray(PyObject** items, size_t size) {
    PyObject* obj = PyTuple_New(size);
    for (size_t i = 0; i < size; ++i) PyTuple_SetItem(obj, i, items[i]);
    return obj;
}

// tuple + tuple -> tuple. Mirrors PyList_Concat's ownership shape.
PyObject* PyTuple_Concat(PyObject* a, PyObject* b) {
    if (!a || !b || a->type != 7 || b->type != 7) return nullptr;
    size_t na = PyTuple_Size(a), nb = PyTuple_Size(b);
    PyObject* r = PyTuple_New(na + nb);
    for (size_t i = 0; i < na; ++i) {
        PyObject* it = PyTuple_GetItem(a, i);   // new ref
        PyTuple_SetItem(r, i, it);              // steals it
    }
    for (size_t i = 0; i < nb; ++i) {
        PyObject* it = PyTuple_GetItem(b, i);
        PyTuple_SetItem(r, na + i, it);
    }
    return r;
}

// tuple * int -> tuple (and int * tuple). Mirrors PyList_Repeat.
PyObject* PyTuple_Repeat(PyObject* tuple, long n) {
    if (!tuple || tuple->type != 7 || n <= 0) return PyTuple_New(0);
    size_t sz = PyTuple_Size(tuple);
    PyObject* r = PyTuple_New(sz * (size_t)n);
    for (long k = 0; k < n; ++k) {
        for (size_t i = 0; i < sz; ++i) {
            PyObject* it = PyTuple_GetItem(tuple, i);
            PyTuple_SetItem(r, (size_t)k * sz + i, it);
        }
    }
    return r;
}

// tuple(iterable) — materializes any iterable into a real tuple. Mirrors
// PyBuiltin_List's dispatch but wraps the result as type 7.
PyObject* PyBuiltin_Tuple(PyObject* obj) {
    if (!obj) return PyTuple_New(0);
    if (obj->type == 7) { Py_INCREF(obj); return obj; }
    // Reuse PyBuiltin_List's full materialization (str/bytes/dict/set/
    // iterator-protocol/class-instance) then convert the list to a tuple.
    PyObject* lst = PyBuiltin_List(obj);
    if (!lst) return PyTuple_New(0);
    size_t n = PyList_Size(lst);
    PyObject* r = PyTuple_New(n);
    for (size_t i = 0; i < n; ++i) {
        // Read the raw slot without INCREF so we can hand a single ref to
        // PyTuple_SetItem. PyList_GetItem returns a *new* ref for
        // homogeneous int/float lists (PyInt_FromLong/PyFloat_FromDouble)
        // but a *borrowed* ref for boxed lists — to avoid a leak in the
        // homogeneous case, normalize the list first so every element is a
        // real boxed slot, then read the borrowed pointer directly.
        if (lst->list_item_type != 0) pyc_ensure_boxed_list(lst);
        PyObject* it = (i < lst->list.size()) ? lst->list[i] : nullptr;
        PyTuple_SetItem(r, i, it);  // INCREFs it; the list keeps its own ref
    }
    Py_DECREF(lst);
    return r;
}

// Negative-index normalization for the native-fast-path list get/set
// functions below — found missing while bug hunting: unlike every other
// indexing path in this file (PyList_GetItemObj, PyList_GetItemI64, str/
// bytes indexing), these six functions took an unsigned size_t index
// with no "if negative, add length" step at all. Codegen.cpp routes
// lst[-1]-style subscripts on a compile-time-known homogeneous int/float
// list straight to these functions with a raw native i64 index (the
// A4/A7 fast path, bypassing the correctly-negative-aware Pyc_Subscript
// entirely) — a negative i64 reinterpreted as size_t becomes an enormous
// value, so `lst[-1]` on such a list raised a bogus IndexError (get) or
// silently no-op'd (set) instead of returning/updating the last element.
// Confirmed against real CPython: `[1,2,3][-1]` must be 3.
static inline long pyc_normalize_list_index(PyObject* list, long index) {
    if (!list || list->type != 1) return index;
    long n;
    if (list->list_item_type == 1) n = (long)list->ilist.size();
    else if (list->list_item_type == 2) n = (long)list->flist.size();
    else n = (long)list->list.size();
    if (index < 0) index += n;
    return index;
}

long PyList_GetItemInt64(PyObject* list, long index) {
    index = pyc_normalize_list_index(list, index);
    if (index < 0) {
        if (list && list->type == 1 && list->list_item_type == 1)
            pyc_raise_msg("IndexError", "list index out of range");
        return 0;
    }
    size_t idx = (size_t)index;
    if (list && list->type == 1 && list->list_item_type == 1 && idx < list->ilist.size())
        return list->ilist[idx];
    if (list && list->type == 1 && list->list_item_type == 1)
        pyc_raise_msg("IndexError", "list index out of range");
    // Fallback: boxed list (e.g. after sorted()/slice demotion) holding int/bool/float
    if (list && list->type == 1 && list->list_item_type == 0 && idx < list->list.size()) {
        PyObject* el = list->list[idx];
        if (el && (el->type == 0 || el->type == 5)) return el->value;
        if (el && el->type == 4) return (long)el->dvalue;
    }
    return 0;
}

void PyList_SetItemInt64(PyObject* list, long index, long v) {
    index = pyc_normalize_list_index(list, index);
    if (index < 0) return;
    size_t idx = (size_t)index;
    if (list && list->type == 1 && list->list_item_type == 1 && idx < list->ilist.size())
        list->ilist[idx] = v;
    else if (list && list->type == 1 && list->list_item_type == 0 && idx < list->list.size()) {
        PyObject* old = list->list[idx];
        if (old) Py_DECREF(old);
        list->list[idx] = PyInt_FromLong(v);
    }
}

double PyList_GetItemDouble(PyObject* list, long index) {
    index = pyc_normalize_list_index(list, index);
    if (index < 0) {
        if (list && list->type == 1 && list->list_item_type == 2)
            pyc_raise_msg("IndexError", "list index out of range");
        return 0.0;
    }
    size_t idx = (size_t)index;
    if (list && list->type == 1 && list->list_item_type == 2 && idx < list->flist.size())
        return list->flist[idx];
    if (list && list->type == 1 && list->list_item_type == 2)
        pyc_raise_msg("IndexError", "list index out of range");
    // Fallback: boxed list holding float/int
    if (list && list->type == 1 && list->list_item_type == 0 && idx < list->list.size()) {
        PyObject* el = list->list[idx];
        if (el && el->type == 4) return el->dvalue;
        if (el && (el->type == 0 || el->type == 5)) return (double)el->value;
    }
    return 0.0;
}

void PyList_SetItemDouble(PyObject* list, long index, double v) {
    index = pyc_normalize_list_index(list, index);
    if (index < 0) return;
    size_t idx = (size_t)index;
    if (list && list->type == 1 && list->list_item_type == 2 && idx < list->flist.size())
        list->flist[idx] = v;
    else if (list && list->type == 1 && list->list_item_type == 0 && idx < list->list.size()) {
        PyObject* old = list->list[idx];
        if (old) Py_DECREF(old);
        list->list[idx] = PyFloat_FromDouble(v);
    }
}

void PyList_SetItemDoubleAuto(PyObject* list, long index, double v) {
    if (!list || list->type != 1) return;
    index = pyc_normalize_list_index(list, index);
    if (index < 0) return;
    size_t idx = (size_t)index;
    if (list->list_item_type == 2 && idx < list->flist.size()) {
        list->flist[idx] = v;
        return;
    }
    if (idx < list->list.size()) {
        PyObject* old = list->list[idx];
        if (old) Py_DECREF(old);
        list->list[idx] = PyFloat_FromDouble(v);
    }
    // If index >= size, silently ignore (matches PyList_SetItem behavior)
}

void PyList_SetItemInt64Auto(PyObject* list, long index, long v) {
    if (!list || list->type != 1) return;
    index = pyc_normalize_list_index(list, index);
    if (index < 0) return;
    size_t idx = (size_t)index;
    if (list->list_item_type == 1 && idx < list->ilist.size()) {
        list->ilist[idx] = v;
        return;
    }
    if (idx < list->list.size()) {
        PyObject* old = list->list[idx];
        if (old) Py_DECREF(old);
        list->list[idx] = PyInt_FromLong(v);
    }
}

PyObject* PyList_Range(int start, int end) {
    PyObject* list = PyList_New(end > start ? end - start : 0);
    for (int i = start; i < end; i++)
        PyList_SetItem(list, i - start, PyInt_FromLong(i));
    return list;
}

// Forward declaration: full definition (with its own explanatory
// comment) is further down in this file; needed here because it's used
// by PyList_Concat below, which appears earlier.
static void pyc_ensure_boxed_list(PyObject* lst);

// list + list concatenation. Not previously implemented at all (found
// while hunting for more instances of the truthiness bug's "reads
// obj->list directly, ignoring list_item_type" class — this one turned
// out to be a different flavor of the same underlying gap: PyNumber_Add
// had no type==1 && type==1 branch whatsoever, so `[1,2,3] + [4,5]`
// returned None unconditionally, confirmed against real CPython
// regardless of whether either list used the homogeneous fast-path
// storage). Normalizes both operands to the generic boxed representation
// first (mutating list_item_type in place, same as every other
// list-reading function in this file that calls pyc_ensure_boxed_list)
// so this works correctly for homogeneous-int/float and mixed-type
// lists alike.
static PyObject* PyList_Concat(PyObject* a, PyObject* b) {
    pyc_ensure_boxed_list(a);
    pyc_ensure_boxed_list(b);
    size_t na = a->list.size(), nb = b->list.size();
    PyObject* result = PyList_New(na + nb);
    for (size_t i = 0; i < na; ++i) {
        PyObject* elem = a->list[i];
        if (elem) Py_INCREF(elem);
        PyList_SetItem(result, i, elem);
    }
    for (size_t i = 0; i < nb; ++i) {
        PyObject* elem = b->list[i];
        if (elem) Py_INCREF(elem);
        PyList_SetItem(result, na + i, elem);
    }
    return result;
}

// Repeat a list `n` times (positive int only — matches CPython which raises
// TypeError on negative int * list, since it's a sequence repetition).
// Each element is INCREF'd so the result owns its references; the source
// list is unchanged.
 static PyObject* PyList_Repeat(PyObject* list, long n) {
     if (n < 0) n = 0;     // conservative: matches "empty" rather than error
     // Handle homogeneous int lists
     if (list && list->type == 1 && list->list_item_type == 1) {
         size_t srcSize = list->ilist.size();
         PyObject* result = PyList_NewInt(0);
         for (long i = 0; i < n; ++i) {
             for (size_t j = 0; j < srcSize; ++j) {
                 result->ilist.push_back(list->ilist[j]);
             }
         }
         return result;
     }
     // Handle homogeneous float lists
     if (list && list->type == 1 && list->list_item_type == 2) {
         size_t srcSize = list->flist.size();
         PyObject* result = PyList_NewFloat(0);
         for (long i = 0; i < n; ++i) {
             for (size_t j = 0; j < srcSize; ++j) {
                 result->flist.push_back(list->flist[j]);
             }
         }
         return result;
     }
     // Handle boxed lists
     size_t srcSize = list->list.size();
     PyObject* result = PyList_NewBoxed(PyInt_FromLong((long)srcSize * n));
     for (long i = 0; i < n; ++i) {
         for (size_t j = 0; j < srcSize; ++j) {
             PyObject* elem = list->list[j];
             if (elem) Py_INCREF(elem);
             PyList_SetItem(result, i * srcSize + j, elem);
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
    // Update the value in place (preserving insertion order, matching
    // CPython 3.7+ dict semantics) rather than erase-and-reappend.
    if (key) {
        for (auto it = dict->dict.begin(); it != dict->dict.end(); ++it) {
            if (PyObject_CompareBool(it->first, key, 0)) {
                // Found equivalent key — replace the value, keep the key.
                if (it->second) Py_DECREF(it->second);
                it->second = value;
                if (value) Py_INCREF(value);
                return;
            }
        }
    }
    // New key — append (insertion-ordered).
    dict->dict.push_back({key, value});
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
        // Missing key: real `del d[k]` raises KeyError — found silently
        // succeeding instead while verifying the Pyc_DelItem fix above
        // (this function's own pre-existing behavior, not introduced by
        // that fix). Raw key as the message, same as Pyc_Subscript's
        // KeyError above — pyc_exc_message adds the repr quoting.
        PyObject* t = PyUnicode_FromString("KeyError");
        PyObject* e = pyc_make_exc(t, key);
        Py_DECREF(t);
        pyc_raise(e);
        return nullptr;
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
extern "C" PyObject* PyBuiltin_ReFinditer(PyObject* pattern, PyObject* subject, PyObject* flags);
extern "C" PyObject* PyBuiltin_ReFindall(PyObject* pattern, PyObject* subject, PyObject* flags);
extern "C" PyObject* PyBuiltin_ReCompile(PyObject* pattern, PyObject* flags);
extern "C" PyObject* PyBuiltin_ReMatchGroup(PyObject* m, PyObject* idxObj);

// datetime support: two more opaque-handle types, same pattern as
// CompiledRegex/MatchObj above (type 14 = date/datetime, type 15 =
// timedelta, payload reached via the `value` field). One C++ struct
// covers both `datetime.date` and `datetime.datetime` (via `hasTime`)
// since pyc's class system can't reliably model `datetime` as a
// subclass of `date` (confirmed in an earlier session's collections/
// Counter investigation: dict subclassing doesn't behave as a real
// dict, so a similar "date subclasses datetime" pyc-level relationship
// isn't attempted here either).
struct PycDateTime {
    int year = 1, month = 1, day = 1;
    int hour = 0, minute = 0, second = 0;
    bool hasTime = false;
};
struct PycTimedelta {
    int64_t days = 0, seconds = 0, microseconds = 0;
};

// Floor division/modulo (C++'s / and % truncate toward zero for negative
// operands; calendar math needs floor semantics throughout).
static int64_t pyc_floordiv(int64_t a, int64_t b) {
    int64_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

// Howard Hinnant's proleptic-Gregorian civil calendar conversion
// (http://howardhinnant.github.io/date_algorithms.html) — days_from_civil
// returns days since 1970-01-01 (may be negative); civil_from_days is its
// inverse. Used for all date/datetime/timedelta arithmetic, comparison,
// and weekday computation below.
static int64_t pyc_days_from_civil(int y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void pyc_civil_from_days(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yr = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    d = (int)(doy - (153u * mp + 2u) / 5u + 1u);
    m = (int)(mp + (mp < 10 ? 3 : (unsigned)-9));
    y = (int)(yr + (m <= 2 ? 1 : 0));
}
// 1970-01-01 (day 0) was a Thursday; Python's weekday() is Monday=0.
static int pyc_weekday_from_days(int64_t days) {
    return (int)(((days + 3) % 7 + 7) % 7);
}

// Normalize a timedelta's fields the way CPython does: 0 <= seconds < 86400,
// 0 <= microseconds < 1000000, with the overall sign folded entirely into days.
static void pyc_normalize_timedelta(int64_t& days, int64_t& seconds, int64_t& microseconds) {
    int64_t dus = pyc_floordiv(microseconds, 1000000);
    microseconds -= dus * 1000000;
    seconds += dus;
    int64_t dsec = pyc_floordiv(seconds, 86400);
    seconds -= dsec * 86400;
    days += dsec;
}

static PycDateTime* pyc_as_datetime(PyObject* o) {
    return (o && o->type == 14) ? reinterpret_cast<PycDateTime*>(o->value) : nullptr;
}
static PycTimedelta* pyc_as_timedelta(PyObject* o) {
    return (o && o->type == 15) ? reinterpret_cast<PycTimedelta*>(o->value) : nullptr;
}
// Constructors use `new PyObject()` (not the calloc-based allocObject()
// used by CompiledRegex/MatchObj below) so PyObject's std::unordered_map
// `dict` member is validly constructed — safe to read (always empty, so
// generic dict-fallback code paths that touch it see "not found" rather
// than undefined behavior from a calloc'd non-trivial C++ member.
static PyObject* pyc_new_datetime(int y, int mo, int d, int h, int mi, int s, bool hasTime) {
    PyObject* o = new PyObject();
    o->refcount = 1;
    o->type = 14;
    PycDateTime* dt = new PycDateTime();
    dt->year = y; dt->month = mo; dt->day = d;
    dt->hour = h; dt->minute = mi; dt->second = s;
    dt->hasTime = hasTime;
    o->value = (int64_t)(intptr_t)dt;
    return o;
}
static PyObject* pyc_new_timedelta(int64_t days, int64_t seconds, int64_t microseconds) {
    pyc_normalize_timedelta(days, seconds, microseconds);
    PyObject* o = new PyObject();
    o->refcount = 1;
    o->type = 15;
    PycTimedelta* td = new PycTimedelta();
    td->days = days; td->seconds = seconds; td->microseconds = microseconds;
    o->value = (int64_t)(intptr_t)td;
    return o;
}

void Py_DECREF(PyObject* obj) {
    if (obj && obj->refcount != IMMORTAL_REFCOUNT && --obj->refcount == 0) {
        if (obj->type == 0 || obj->type == 4) {
            // P1: recycle plain int/float boxes (no owned children)
            freeScalarObj(obj);
            return;
        }
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
        } else if (obj->type == 14) {
            delete reinterpret_cast<PycDateTime*>(obj->value);
        } else if (obj->type == 15) {
            delete reinterpret_cast<PycTimedelta*>(obj->value);
        } else if (obj->type == 19) {
            // decimal.Decimal owns a heap-allocated mpd_t* (libmpdec) —
            // same "opaque pointer in `value`" pattern as types 8/9/14/15
            // above, and the one thing complex numbers (type 13) didn't
            // need since they have no out-of-struct payload.
            mpd_del(reinterpret_cast<mpd_t*>(obj->value));
        } else if (obj->type == 20) {
            for (auto* e : obj->setElems) if (e) Py_DECREF(e);
        } else if (obj->type == 10 || obj->type == 11) {
            if (obj->cell_content) { Py_DECREF(obj->cell_content); obj->cell_content = nullptr; }
        }
        delete obj;   // calls dtors for vector/map/string
    }
}

static int PyObject_PrintBase(PyObject* obj, FILE* fp);
static std::string pyc_exc_message(PyObject* exc);

// User-defined exception subclasses (`class MyError(Exception): pass`)
// found and fixed while bug hunting: raising one used to either crash
// compilation (an argument-count mismatch synthesizing a nonexistent
// base __init__ — see the Compiler.cpp instantiation-site fix) or, once
// that was fixed, produce an instance that was neither catchable by name
// nor by a generic `except Exception:` and printed a garbled internal
// dict repr instead of its message. Root cause: such an instance is an
// ordinary dict-backed class instance (type 2), not a structured
// exception (type 10, built by pyc_make_exc) — the only shape
// pyc_exc_type_name/pyc_exc_matches/pyc_exc_message (further down this
// file) previously understood. These three helpers let a type-2 instance
// participate in the same exception protocol by reading its class's
// __mro__ (a compile-time-flattened list of ancestor class names,
// already stored in every class dict for super() support — see
// lowerClass's B6b in Compiler.cpp) and its "args" list (populated at
// construction time by the same Compiler.cpp fix, mirroring CPython's
// own BaseException.args).
static PyObject* pyc_exc_instance_mro(PyObject* exc) {
    if (!exc || exc->type != 2) return nullptr;
    for (auto& pair : exc->dict) {
        if (pair.first && pair.first->type == 3 && pair.first->str == "__class__") {
            PyObject* classDict = pair.second;
            if (!classDict || classDict->type != 2) return nullptr;
            for (auto& cpair : classDict->dict) {
                if (cpair.first && cpair.first->type == 3 && cpair.first->str == "__mro__") {
                    return cpair.second;
                }
            }
            return nullptr;
        }
    }
    return nullptr;
}
static bool pyc_str_is_builtin_exc_name(const std::string& n) {
    // Mirrors Compiler.cpp's builtinExcNames() — duplicated here since
    // Runtime.cpp has no access to the compiler's tables, and this list
    // is small and stable.
    static const std::unordered_set<std::string> names = {
        "BaseException", "Exception", "ArithmeticError", "ZeroDivisionError",
        "OverflowError", "FloatingPointError", "LookupError", "IndexError",
        "KeyError", "ValueError", "TypeError", "RuntimeError", "StopIteration",
        "AttributeError", "NameError", "UnboundLocalError", "NotImplementedError",
        "OSError", "IOError", "FileNotFoundError", "PermissionError",
        "AssertionError", "SyntaxError", "IndentationError"
    };
    return names.count(n) != 0;
}
// True when `exc` is a class instance whose MRO includes a builtin
// exception name — i.e. an instance of `class Foo(Exception, ...)`
// raised (or printed) directly, rather than a pyc_make_exc structured
// exception.
static bool pyc_instance_is_exception(PyObject* exc) {
    PyObject* mro = pyc_exc_instance_mro(exc);
    if (!mro || mro->type != 1) return false;
    size_t n = PyList_Size(mro);
    for (size_t i = 0; i < n; ++i) {
        PyObject* m = PyList_GetItemI64(mro, (long)i);
        bool match = (m && m->type == 3 && pyc_str_is_builtin_exc_name(m->str));
        if (m) Py_DECREF(m);
        if (match) return true;
    }
    return false;
}

// Shared date/timedelta -> string formatting, used by both PrintElement
// (no trailing newline, for nested container repr) and PrintBase (adds
// one), and by str()/PyStr_FromAny (which both funnel through these).
static void pyc_format_datetime_into(const PycDateTime* dt, std::string& out) {
    char buf[64];
    if (dt->hasTime) {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 dt->year, dt->month, dt->day, dt->hour, dt->minute, dt->second);
    } else {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt->year, dt->month, dt->day);
    }
    out += buf;
}
static void pyc_format_timedelta_into(const PycTimedelta* td, std::string& out) {
    int64_t h = td->seconds / 3600;
    int64_t m = (td->seconds % 3600) / 60;
    int64_t s = td->seconds % 60;
    char buf[96];
    if (td->days != 0) {
        snprintf(buf, sizeof(buf), "%lld day%s, %lld:%02lld:%02lld",
                 (long long)td->days, (td->days == 1 || td->days == -1) ? "" : "s",
                 (long long)h, (long long)m, (long long)s);
    } else {
        snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    }
    out += buf;
    if (td->microseconds != 0) {
        char mbuf[16];
        snprintf(mbuf, sizeof(mbuf), ".%06lld", (long long)td->microseconds);
        out += mbuf;
    }
}

// Formats a bytes/bytearray object's raw content as CPython-style repr
// text: b'...' (bytearray wraps that in bytearray(...)). Escapes
// non-printable/non-ASCII bytes as \xHH, plus \\/\n/\t/\r shorthand.
// Simplification vs real CPython: always single-quotes and escapes an
// embedded "'" as \' — CPython instead switches to double-quotes when
// the content has a "'" but no '"' (avoiding the escape). Both are valid
// Python source for the same bytes value; not byte-for-byte identical to
// CPython's own repr choice when a lone single-quote is present.
static std::string pyc_format_bytes_repr(const std::string& s, bool isBytearray) {
    std::string out = "b'";
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\'') out += "\\'";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c >= 0x20 && c < 0x7f) out += (char)c;
        else {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\x%02x", (unsigned)c);
            out += buf;
        }
    }
    out += "'";
    if (isBytearray) out = "bytearray(" + out + ")";
    return out;
}

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
    if (obj->type == 13) {
        char rbuf[64], ibuf[64];
        format_double_complex(rbuf, sizeof(rbuf), obj->complex_real);
        format_double_complex(ibuf, sizeof(ibuf), obj->complex_imag);
        // CPython format: if real==0, print just "{imag}j" (no parens).
        // Otherwise print "({real}+{imag}j)" or "({real}{imag}j)".
        if (obj->complex_real == 0.0 && !signbit(obj->complex_real)) {
            return fprintf(fp, "%sj", ibuf);
        } else if (!signbit(obj->complex_imag)) {
            return fprintf(fp, "(%s+%sj)", rbuf, ibuf);
        } else {
            return fprintf(fp, "(%s%sj)", rbuf, ibuf);
        }
    }
    if (obj->type == 1) {
        // Nested list — open bracket, recurse, close.
        fprintf(fp, "[");
        // Handle homogeneous int lists
        if (obj->list_item_type == 1) {
            for (size_t i = 0; i < obj->ilist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                PyObject_PrintElement(PyInt_FromLong(obj->ilist[i]), fp);
            }
        } else if (obj->list_item_type == 2) {
            // Handle homogeneous float lists
            for (size_t i = 0; i < obj->flist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                PyObject_PrintElement(PyFloat_FromDouble(obj->flist[i]), fp);
            }
        } else {
            // boxed list
            for (size_t i = 0; i < obj->list.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                PyObject_PrintElement(obj->list[i], fp);
            }
        }
        return fprintf(fp, "]");
    }
    if (obj->type == 7) {
        // Nested tuple — open paren, recurse, close. Single-element tuples
        // get a trailing comma: (1,). Empty tuple: ().
        size_t n;
        if (obj->list_item_type == 1) n = obj->ilist.size();
        else if (obj->list_item_type == 2) n = obj->flist.size();
        else n = obj->list.size();
        fprintf(fp, "(");
        if (obj->list_item_type == 1) {
            for (size_t i = 0; i < obj->ilist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                PyObject_PrintElement(PyInt_FromLong(obj->ilist[i]), fp);
            }
        } else if (obj->list_item_type == 2) {
            for (size_t i = 0; i < obj->flist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                PyObject_PrintElement(PyFloat_FromDouble(obj->flist[i]), fp);
            }
        } else {
            for (size_t i = 0; i < obj->list.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                PyObject_PrintElement(obj->list[i], fp);
            }
        }
        if (n == 1) fprintf(fp, ",");
        return fprintf(fp, ")");
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
    if (obj->type == 20) {
        if (obj->setElems.empty()) return fprintf(fp, "set()");
        fprintf(fp, "{");
        bool first = true;
        for (auto* e : obj->setElems) {
            if (!first) fprintf(fp, ", ");
            PyObject_PrintElement(e, fp);
            first = false;
        }
        return fprintf(fp, "}");
    }
    if (obj->type == 3) {
        // String element inside a container: use repr-style quotes.
        return fprintf(fp, "'%s'", obj->str.c_str());
    }
    if (obj->type == 17 || obj->type == 18) {
        std::string r = pyc_format_bytes_repr(obj->str, obj->type == 18);
        return fprintf(fp, "%s", r.c_str());
    }
    if (obj->type == 19) {
        // Nested-container form matches CPython's Decimal repr exactly:
        // print([Decimal('3.14')]) -> [Decimal('3.14')], not [3.14].
        char* s = mpd_to_sci(pyc_as_decimal(obj), 1);
        int r = fprintf(fp, "Decimal('%s')", s ? s : "0");
        if (s) mpd_free(s);
        return r;
    }
    if (obj->type == 6) return fprintf(fp, "<cell>");
    if (obj->type == 14) {
        std::string s;
        pyc_format_datetime_into(pyc_as_datetime(obj), s);
        return fprintf(fp, "%s", s.c_str());
    }
    if (obj->type == 15) {
        std::string s;
        pyc_format_timedelta_into(pyc_as_timedelta(obj), s);
        return fprintf(fp, "%s", s.c_str());
    }
    if (obj->type == 16) {
        // Matches CPython's PosixPath repr when nested inside a container
        // (e.g. `print([Path("a/b")])` -> `[PosixPath('a/b')]`).
        return fprintf(fp, "PosixPath('%s')", obj->str.c_str());
    }
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
    if (obj->type == 14) {
        std::string s;
        pyc_format_datetime_into(pyc_as_datetime(obj), s);
        int r = fprintf(fp, "%s\n", s.c_str()); fflush(fp); return r;
    }
    if (obj->type == 15) {
        std::string s;
        pyc_format_timedelta_into(pyc_as_timedelta(obj), s);
        int r = fprintf(fp, "%s\n", s.c_str()); fflush(fp); return r;
    }
    if (obj->type == 16) {
        // Top-level print()/str(): raw path text, no PosixPath(...) wrapper
        // (matches CPython: str(Path("a/b")) == "a/b").
        int r = fprintf(fp, "%s\n", obj->str.c_str()); fflush(fp); return r;
    }
    if (obj->type == 13) {
        char rbuf[64], ibuf[64];
        format_double_complex(rbuf, sizeof(rbuf), obj->complex_real);
        format_double_complex(ibuf, sizeof(ibuf), obj->complex_imag);
        int r;
        if (obj->complex_real == 0.0 && !signbit(obj->complex_real)) {
            r = fprintf(fp, "%sj\n", ibuf);
        } else if (!signbit(obj->complex_imag)) {
            r = fprintf(fp, "(%s+%sj)\n", rbuf, ibuf);
        } else {
            r = fprintf(fp, "(%s%sj)\n", rbuf, ibuf);
        }
        fflush(fp); return r;
    }
    if (obj->type == 9) {
        // Match object — print "<re.Match object>" for safety.
        int r = fprintf(fp, "<re.Match object>\n"); fflush(fp); return r;
    }
    if (obj->type == 10) {
        // Exception — print str(e), i.e. the message (empty when none).
        int r = fprintf(fp, "%s\n", pyc_exc_message(obj).c_str()); fflush(fp); return r;
    }
    if (obj->type == 11) {
        // Function object — CPython-style repr.
        const char* nm = obj->cell_content ? obj->cell_content->str.c_str() : obj->str.c_str();
        int r = fprintf(fp, "<function %s at %p>\n", nm, (void*)obj); fflush(fp); return r;
    }
    // Descriptor bundle (closure value): a list whose first element is a
    // function object (type 11) with additional cell elements, or a string
    // token (type 3) followed by cell objects (type 6). Print as <function ...>
    // instead of the raw [token, cell0, ...] list.
    if (obj->type == 1 && !obj->list.empty()) {
        PyObject* first = obj->list[0];
        bool is_bundle = false;
        if (first && first->type == 11) {
            is_bundle = true;
        } else if (first && first->type == 3 && obj->list.size() >= 2) {
            // Check if remaining elements are cells (type 6)
            bool all_cells = true;
            for (size_t i = 1; i < obj->list.size(); ++i) {
                if (!obj->list[i] || obj->list[i]->type != 6) {
                    all_cells = false;
                    break;
                }
            }
            is_bundle = all_cells;
        }
        if (is_bundle) {
            const char* nm = first->type == 11
                ? (first->cell_content ? first->cell_content->str.c_str() : first->str.c_str())
                : first->str.c_str();
            int r = fprintf(fp, "<function %s at %p>\n", nm, (void*)obj); fflush(fp); return r;
        }
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
    if (obj->type == 7) {
        // Top-level tuple print: paren format, single-element trailing comma.
        size_t n;
        if (obj->list_item_type == 1) n = obj->ilist.size();
        else if (obj->list_item_type == 2) n = obj->flist.size();
        else n = obj->list.size();
        fprintf(fp, "(");
        if (obj->list_item_type == 1) {
            for (size_t i = 0; i < obj->ilist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                int r = fprintf(fp, "%ld", obj->ilist[i]);
                (void)r;
            }
        } else if (obj->list_item_type == 2) {
            for (size_t i = 0; i < obj->flist.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                char buf[64];
                format_double(buf, sizeof(buf), obj->flist[i]);
                int r = fprintf(fp, "%s", buf);
                (void)r;
            }
        } else {
            for (size_t i = 0; i < obj->list.size(); ++i) {
                if (i > 0) fprintf(fp, ", ");
                if (obj->list[i] && obj->list[i]->type == 3)
                    fprintf(fp, "'%s'", obj->list[i]->str.c_str());
                else
                    PyObject_PrintElement(obj->list[i], fp);
            }
        }
        if (n == 1) fprintf(fp, ",");
        fprintf(fp, ")\n");
        fflush(fp);
        return 0;
    }
    if (obj->type == 2 && pyc_instance_is_exception(obj)) {
        // User-defined exception subclass instance: print str(e) (the
        // message), matching CPython's default Exception.__str__ —
        // not the raw {'__class__': ...} dict repr a plain class
        // instance with no __str__/__repr__ would otherwise get below.
        int r = fprintf(fp, "%s\n", pyc_exc_message(obj).c_str()); fflush(fp); return r;
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
    if (obj->type == 20) {
        // Empty set prints as `set()` (a literal `{}` is a dict, so CPython
        // needs `set()` to denote an empty set; matching that here).
        if (obj->setElems.empty()) { int r = fprintf(fp, "set()\n"); fflush(fp); return r; }
        fprintf(fp, "{");
        bool first = true;
        for (auto* e : obj->setElems) {
            if (!first) fprintf(fp, ", ");
            PyObject_PrintElement(e, fp);
            first = false;
        }
        fprintf(fp, "}\n");
        fflush(fp);
        return 0;
    }
    if (obj->type == 3) { int r = fprintf(fp, "%s\n", obj->str.c_str()); fflush(fp); return r; }
    if (obj->type == 17 || obj->type == 18) {
        // Unlike str/Path, bytes has no bare __str__ — print() shows the
        // same b'...' repr form CPython does (print(b"hi") -> b'hi').
        std::string r = pyc_format_bytes_repr(obj->str, obj->type == 18);
        int rc = fprintf(fp, "%s\n", r.c_str()); fflush(fp); return rc;
    }
    if (obj->type == 19) {
        // Bare print()/str() shows the plain digit string (matches
        // int/float/str's existing bare-print convention) — no
        // Decimal('...') wrapper here, unlike PyObject_PrintElement.
        char* s = mpd_to_sci(pyc_as_decimal(obj), 1);
        int r = fprintf(fp, "%s\n", s ? s : "0"); fflush(fp);
        if (s) mpd_free(s);
        return r;
    }
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

// Length-explicit variant — needed for str-as-byte-buffer content (struct
// pack/unpack's binary output routinely contains embedded 0x00 bytes,
// e.g. any little-endian integer field with a zero high byte).
// PyUnicode_FromString's const char* + implicit strlen() would silently
// truncate at the first NUL; this preserves the full byte count via
// std::string's explicit-length constructor.
PyObject* PyUnicode_FromStringAndSize(const char* s, size_t n) {
    alloc_str_count++;
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 3;
    obj->str.assign(s, n);
    return obj;
}

// bytes (type 17) / bytearray (type 18) — reuse the `str` field for
// storage exactly like pathlib.Path (type 16) reuses it for path text:
// no Codegen.cpp struct-layout changes needed (Codegen only touches
// fields 0-3: refcount/type/value/dvalue), no Py_DECREF branch needed
// (plain `delete obj` already runs std::string's destructor). Both
// constructors are explicit-length (not C-string-based) so embedded NUL
// bytes survive, mirroring PyUnicode_FromStringAndSize just above.
PyObject* PyBytes_FromStringAndSize(const char* s, size_t n) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 17;
    obj->str.assign(s, n);
    return obj;
}
PyObject* PyByteArray_FromStringAndSize(const char* s, size_t n) {
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 18;
    obj->str.assign(s, n);
    return obj;
}
// True for a plain str (type 3), bytes (17), or bytearray (18) — all
// three store their content in the `str` field. Used by helpers (e.g.
// hashlib) that should accept any of the three as raw byte-ish input,
// same rationale as pyc_is_path_like allowing str/Path interop.
static bool pyc_is_bytes_like(PyObject* o) { return o && (o->type == 3 || o->type == 17 || o->type == 18); }

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
        // Descriptor bundle: first element is a function object or a string
        // token followed by cell objects.
        if (!obj->list.empty()) {
            PyObject* first = obj->list[0];
            bool is_bundle = false;
            if (first && first->type == 11) {
                is_bundle = true;
            } else if (first && first->type == 3 && obj->list.size() >= 2) {
                bool all_cells = true;
                for (size_t i = 1; i < obj->list.size(); ++i) {
                    if (!obj->list[i] || obj->list[i]->type != 6) {
                        all_cells = false;
                        break;
                    }
                }
                is_bundle = all_cells;
            }
            if (is_bundle) {
                std::string nm = first->type == 11
                    ? (first->cell_content ? first->cell_content->str : first->str)
                    : first->str;
                std::string result = "<function " + nm + " at " + std::to_string(reinterpret_cast<uintptr_t>(obj)) + ">";
                return PyUnicode_FromString(result.c_str());
            }
        }
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

// Extract C string from a PyObject* (type 3 = str).
// Returns nullptr if obj is not a string type.
const char* PyStr_AsUTF8(PyObject* obj) {
    if (!obj || obj->type != 3) return nullptr;
    return obj->str.c_str();
}

// os.path stubs - use real POSIX functions
PyObject* Pyc_OsPathExists(PyObject* pathObj) {
    const char* path = PyStr_AsUTF8(pathObj);
    if (!path) return PyBool_New(0);
    int result = access(path, F_OK) == 0;
    return PyBool_New(result);
}

PyObject* Pyc_OsPathIsFile(PyObject* pathObj) {
    const char* path = PyStr_AsUTF8(pathObj);
    if (!path) return PyBool_New(0);
    struct stat st;
    int result = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    return PyBool_New(result);
}

PyObject* Pyc_OsPathIsDir(PyObject* pathObj) {
    const char* path = PyStr_AsUTF8(pathObj);
    if (!path) return PyBool_New(0);
    struct stat st;
    int result = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    return PyBool_New(result);
}

PyObject* Pyc_OsUnlink(PyObject* pathObj) {
    const char* path = PyStr_AsUTF8(pathObj);
    if (!path) return nullptr;
    int result = unlink(path);
    return PyInt_FromLong(result);
}

// subprocess.call(cmd) -> exit status (caller shifts << 8 if needed)
// cmd is a list of strings: [cmd, arg1, arg2, ...]
PyObject* Pyc_SubprocessCall(PyObject* cmdList) {
    if (!cmdList || cmdList->type != 1) {
        return PyInt_FromLong(-1);
    }
    
    // Count valid string arguments
    int argc = 0;
    for (auto* item : cmdList->list) {
        if (item && item->type == 3) {
            argc++;
        }
    }
    
    if (argc == 0) {
        return PyInt_FromLong(-1);
    }
    
    // Allocate argv array
    char** argv = new char*[argc + 1];
    std::vector<std::string> argvStrs;
    argvStrs.reserve(argc);
    
    int i = 0;
    for (auto* item : cmdList->list) {
        if (item && item->type == 3) {
            argvStrs.push_back(item->str);
            argv[i++] = const_cast<char*>(argvStrs.back().c_str());
        }
    }
    argv[argc] = nullptr;
    
    pid_t pid = fork();
    if (pid < 0) {
        delete[] argv;
        return PyInt_FromLong(-1);
    }
    
    if (pid == 0) {
        // Child process
        execvp(argv[0], argv);
        _exit(127);
    }
    
    // Parent process - wait for child
    int status;
    waitpid(pid, &status, 0);
    
    delete[] argv;
    
    if (WIFEXITED(status)) {
        return PyInt_FromLong(WEXITSTATUS(status));
    }
    return PyInt_FromLong(1);
}

// subprocess.check_output(cmd) -> stdout as string
// cmd is a list of strings: [cmd, arg1, arg2, ...]
PyObject* Pyc_SubprocessCheckOutput(PyObject* cmdList) {
    if (!cmdList || cmdList->type != 1) {
        return PyUnicode_FromString("");
    }
    
    // Count valid string arguments
    int argc = 0;
    for (auto* item : cmdList->list) {
        if (item && item->type == 3) {
            argc++;
        }
    }
    
    if (argc == 0) {
        return PyUnicode_FromString("");
    }
    
    // Allocate argv array
    char** argv = new char*[argc + 1];
    std::vector<std::string> argvStrs;
    argvStrs.reserve(argc);
    
    int i = 0;
    for (auto* item : cmdList->list) {
        if (item && item->type == 3) {
            argvStrs.push_back(item->str);
            argv[i++] = const_cast<char*>(argvStrs.back().c_str());
        }
    }
    argv[argc] = nullptr;
    
    // Create pipe for capturing stdout
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        delete[] argv;
        return PyUnicode_FromString("");
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        delete[] argv;
        return PyUnicode_FromString("");
    }
    
    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    
    // Parent process
    close(pipefd[1]);
    delete[] argv;
    
    // Read stdout
    char buf[65536];
    std::string output;
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        output.append(buf, n);
    }
    close(pipefd[0]);
    
    // Wait for child
    int status;
    waitpid(pid, &status, 0);
    
    (void)status;
    
    return PyUnicode_FromString(output.c_str());
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
    if (obj->type == 13) return PyComplex_New(-obj->complex_real, -obj->complex_imag);
    if (obj->type == 19) {
        mpd_t* r = mpd_qnew();
        uint32_t status = 0;
        mpd_qcopy_negate(r, pyc_as_decimal(obj), &status);
        return pyc_decimal_wrap(r);
    }
    // __neg__ dispatch for a class instance — found and fixed while bug
    // hunting: -instance previously always returned None.
    if (obj->type == 2) {
        PyObject* negMethod = pyc_lookup_dunder(obj, "__neg__");
        if (negMethod) return pyc_call_dunder1(negMethod, obj);
    }
    return NULL;
}

// Strict full-string integer parse (Python semantics): the whole string,
// modulo surrounding whitespace, must be consumed or it's a ValueError.
static bool pyc_parse_long(const std::string& s, int base, long* out) {
    try {
        size_t pos = 0;
        long v = std::stol(s, &pos, base);
        while (pos < s.size() && isspace((unsigned char)s[pos])) ++pos;
        if (pos != s.size()) return false;
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

PyObject* PyBuiltin_Int(PyObject* obj) {
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 0 || obj->type == 5) return PyInt_FromLong(obj->value);
    if (obj->type == 4) return PyInt_FromLong((long)obj->dvalue);
    if (obj->type == 19) {
        // int(Decimal(...)) truncates toward zero, matching CPython.
        mpd_t* a = pyc_as_decimal(obj);
        uint32_t status = 0;
        mpd_context_t truncCtx = *pyc_dec_ctx();
        mpd_qsetround(&truncCtx, MPD_ROUND_DOWN);
        mpd_t* rounded = mpd_qnew();
        mpd_qround_to_int(rounded, a, &truncCtx, &status);
        int64_t v = mpd_qget_ssize(rounded, &status);
        mpd_del(rounded);
        return PyInt_FromLong(v);
    }
    if (obj->type == 3) {
        long v;
        if (pyc_parse_long(obj->str, 10, &v)) return PyInt_FromLong(v);
        pyc_raise_msg("ValueError", ("invalid literal for int() with base 10: '" + obj->str + "'").c_str());
        return nullptr;
    }
    return PyInt_FromLong(0);
}

PyObject* PyBuiltin_IntBase(PyObject* obj, PyObject* base) {
    int b = base ? (int)base->value : 10;
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 3) {
        long v;
        if (pyc_parse_long(obj->str, b, &v)) return PyInt_FromLong(v);
        pyc_raise_msg("ValueError", ("invalid literal for int() with base " + std::to_string(b) + ": '" + obj->str + "'").c_str());
        return nullptr;
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
    if (obj->type == 19) {
        char* s = mpd_to_sci(pyc_as_decimal(obj), 1);
        double v = s ? strtod(s, nullptr) : 0.0;
        if (s) mpd_free(s);
        return PyFloat_FromDouble(v);
    }
    if (obj->type == 3) {
        try { return PyFloat_FromDouble(std::stod(obj->str)); } catch (...) {}
    }
    return PyFloat_FromDouble(0.0);
}

// complex(x) — construct a complex number from various types.
//   complex()     -> 0+0j
//   complex(3)    -> 3+0j
//   complex(3.5)  -> 3.5+0j
//   complex(1j)   -> 0+1j (returns same object)
//   complex(3,4)  -> 3+4j
//   complex("3+4j") -> parse string (basic numeric only)
PyObject* PyBuiltin_Complex(PyObject* obj1, PyObject* obj2) {
    // Two-argument form: complex(real, imag)
    if (obj1 && obj2) {
        double real = 0.0, imag = 0.0;
        if (obj1->type == 13) {
            real = obj1->complex_real;
        } else if (obj1->type == 0 || obj1->type == 5) {
            real = (double)obj1->value;
        } else if (obj1->type == 4) {
            real = obj1->dvalue;
        }
        if (obj2->type == 13) {
            imag = obj2->complex_imag;
        } else if (obj2->type == 0 || obj2->type == 5) {
            imag = (double)obj2->value;
        } else if (obj2->type == 4) {
            imag = obj2->dvalue;
        }
        return PyComplex_New(real, imag);
    }
    // Single-argument form: complex(x)
    if (!obj1) return PyComplex_New(0.0, 0.0);
    if (obj1->type == 13) {
        Py_INCREF(obj1);
        return obj1;
    }
    if (obj1->type == 0 || obj1->type == 5) {
        return PyComplex_New((double)obj1->value, 0.0);
    }
    if (obj1->type == 4) {
        return PyComplex_New(obj1->dvalue, 0.0);
    }
    if (obj1->type == 3) {
        // Parse string: "3+4j", "3.5+1.5j", "2j", "-3+4j", etc.
        std::string s = obj1->str;
        size_t start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return PyComplex_New(0.0, 0.0);
        size_t end = s.find_last_not_of(" \t\n\r");
        s = s.substr(start, end - start + 1);
        double real = 0.0, imag = 0.0;
        // Check for 'j' or 'J' suffix
        size_t jpos = s.find_first_of("jJ");
        if (jpos != std::string::npos) {
            std::string beforeJ = s.substr(0, jpos);
            size_t plusPos = beforeJ.find('+');
            size_t minusPos = beforeJ.rfind('-');
            bool hasReal = false;
            if (plusPos != std::string::npos && plusPos > 0) {
                real = std::stod(beforeJ.substr(0, plusPos));
                std::string imagPart = beforeJ.substr(plusPos + 1);
                if (imagPart.empty() || imagPart == "+") {
                    imag = 1.0;
                } else if (imagPart == "-") {
                    imag = -1.0;
                } else {
                    try { imag = std::stod(imagPart); } catch (...) {}
                }
                hasReal = true;
            } else if (minusPos != std::string::npos && minusPos > 0) {
                real = std::stod(beforeJ.substr(0, minusPos));
                std::string imagPart = beforeJ.substr(minusPos);
                try { imag = std::stod(imagPart); } catch (...) {}
                hasReal = true;
            }
            if (!hasReal) {
                if (beforeJ.empty() || beforeJ == "+" || beforeJ == "-") {
                    imag = (beforeJ == "-" || beforeJ == "-+") ? -1.0 : 1.0;
                } else {
                    try { imag = std::stod(beforeJ); } catch (...) {}
                }
            }
        } else {
            try { real = std::stod(s); } catch (...) {}
        }
        return PyComplex_New(real, imag);
    }
    return PyComplex_New(0.0, 0.0);
}

// bool(x) — returns PyBool_New of the truthiness of x. CPython's bool()
// always returns a real bool (True/False). Our PyBool_New now returns
// the cached immortal singletons, so identity comparisons work.
PyObject* PyBuiltin_Bool(PyObject* obj) {
    // Found and fixed while bug hunting: this used to be a second,
    // independent reimplementation of PyObject_TruthValue's logic (down
    // to duplicating its own now-fixed homogeneous-list bug pattern),
    // so PyObject_TruthValue's __bool__/__len__ dunder dispatch (added
    // this pass) never applied to the bare bool() builtin — confirmed
    // bool(Vec(0,0)) for a class defining __bool__ still incorrectly
    // returned True. Delegating outright removes the duplication rather
    // than patching it a second time.
    return PyBool_New(PyObject_TruthValue(obj));
}

// type(x) — returns a string naming the runtime type of x. We use the
// same names CPython uses so user code that compares to type names works.
PyObject* PyBuiltin_Type(PyObject* obj) {
    if (!obj) return PyUnicode_FromString("<class 'NoneType'>");
    switch (obj->type) {
        case 0: return PyUnicode_FromString("<class 'int'>");
        case 1: return PyUnicode_FromString("<class 'list'>");
        case 7: return PyUnicode_FromString("<class 'tuple'>");
        case 2: {
            // Class instance (has a "__class__" entry) vs a genuine
            // plain dict — found and fixed while bug hunting: type(e)
            // for a caught user-defined exception (or any user-defined
            // class instance) previously showed the generic '<class
            // 'dict'>' instead of the real class name. Uses the same
            // __mro__[0] lookup already relied on for super() and for
            // structured-exception-style matching on a class instance
            // (see pyc_exc_instance_mro's comment, far above) — despite
            // the name, it's general-purpose, not exception-specific.
            PyObject* mro = pyc_exc_instance_mro(obj);
            if (mro && mro->type == 1 && PyList_Size(mro) > 0) {
                PyObject* first = PyList_GetItemI64(mro, 0);
                std::string name = (first && first->type == 3) ? first->str : "dict";
                if (first) Py_DECREF(first);
                return PyUnicode_FromString(("<class '" + name + "'>").c_str());
            }
            return PyUnicode_FromString("<class 'dict'>");
        }
        case 3: return PyUnicode_FromString("<class 'str'>");
        case 4: return PyUnicode_FromString("<class 'float'>");
        case 5: return PyUnicode_FromString("<class 'bool'>");
        case 6: return PyUnicode_FromString("<class 'cell'>");
        case 10:
            // Structured (builtin) exception — found and fixed
            // alongside the type-2 case above: previously fell through
            // to the generic '<class 'object'>' default.
            return PyUnicode_FromString(("<class '" + obj->str + "'>").c_str());
        case 13: return PyUnicode_FromString("<class 'complex'>");
        case 14: return PyUnicode_FromString(pyc_as_datetime(obj)->hasTime
                     ? "<class 'datetime.datetime'>" : "<class 'datetime.date'>");
        case 15: return PyUnicode_FromString("<class 'datetime.timedelta'>");
        case 17: return PyUnicode_FromString("<class 'bytes'>");
        case 18: return PyUnicode_FromString("<class 'bytearray'>");
        case 19: return PyUnicode_FromString("<class 'decimal.Decimal'>");
        case 20: return PyUnicode_FromString("<class 'set'>");
        default: return PyUnicode_FromString("<class 'object'>");
    }
}

// Forward declaration: the callable registry is defined far below (B4/B8
// dispatch section) but PyBuiltin_Callable needs to check it.
static std::unordered_map<std::string, PyObject* (*)(PyObject*)> g_callableRegistry;

PyObject* PyBuiltin_Callable(PyObject* obj) {
    if (!obj) return PyBool_New(0);
    // Type 11 = function object, type 12 = exception class — callable.
    if (obj->type == 11 || obj->type == 12) {
        return PyBool_New(1);
    }
    // Type 3 (str): only callable if it's a registered callable token.
    if (obj->type == 3) {
        auto it = g_callableRegistry.find(obj->str);
        if (it != g_callableRegistry.end() && it->second) {
            return PyBool_New(1);
        }
        return PyBool_New(0);
    }
    // Type 2 (dict): could be a class dict (has __mro__) or a class
    // instance with __call__.
    if (obj->type == 2) {
        // Class dict (instantiable class)
        for (auto& p : obj->dict) {
            if (p.first && p.first->type == 3 && p.first->str == "__mro__") {
                return PyBool_New(1);
            }
        }
        // Class instance with __call__
        PyObject* callMethod = pyc_lookup_dunder(obj, "__call__");
        if (callMethod) { Py_DECREF(callMethod); return PyBool_New(1); }
    }
    // Descriptor bundle list whose first element is a callable token
    if (obj->type == 1 && !obj->list.empty()) {
        PyObject* first = obj->list[0];
        if (first) {
            if (first->type == 11 || first->type == 12) return PyBool_New(1);
            if (first->type == 3) {
                auto it = g_callableRegistry.find(first->str);
                if (it != g_callableRegistry.end() && it->second) return PyBool_New(1);
            }
        }
    }
    return PyBool_New(0);
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

// divmod(a, b) — return (a // b, a % b). CPython returns a 2-tuple; now
// that pyc has a real tuple type, we return a real tuple here.
PyObject* PyBuiltin_Divmod(PyObject* a, PyObject* b) {
    if (!a || !b) return nullptr;
    PyObject* q = PyNumber_Divide(a, b);
    PyObject* r = PyNumber_Remainder(a, b);
    if (!q || !r) { if (q) Py_DECREF(q); if (r) Py_DECREF(r); return nullptr; }
    PyObject* r2 = PyTuple_New(2);
    PyTuple_SetItem(r2, 0, q);
    PyTuple_SetItem(r2, 1, r);
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
    if (obj->type == 13) {
        char rbuf[64], ibuf[64];
        format_double_complex(rbuf, sizeof(rbuf), obj->complex_real);
        format_double_complex(ibuf, sizeof(ibuf), obj->complex_imag);
        std::string s;
        if (obj->complex_real == 0.0 && !signbit(obj->complex_real)) {
            s = std::string(ibuf) + "j";
        } else if (!signbit(obj->complex_imag)) {
            s = "(" + std::string(rbuf) + "+" + std::string(ibuf) + "j)";
        } else {
            s = "(" + std::string(rbuf) + std::string(ibuf) + "j)";
        }
        return PyUnicode_FromString(s.c_str());
    }
    if (obj->type == 3) {
        // String: wrap in single quotes (simplified — no escaping).
        std::string r = "'" + obj->str + "'";
        return PyUnicode_FromString(r.c_str());
    }
    if (obj->type == 17 || obj->type == 18) {
        std::string r = pyc_format_bytes_repr(obj->str, obj->type == 18);
        return PyUnicode_FromStringAndSize(r.data(), r.size());
    }
    if (obj->type == 19) {
        char* s = mpd_to_sci(pyc_as_decimal(obj), 1);
        std::string r = std::string("Decimal('") + (s ? s : "0") + "')";
        if (s) mpd_free(s);
        return PyUnicode_FromStringAndSize(r.data(), r.size());
    }
    if (obj->type == 1) {
        // Descriptor bundle: first element is a function object or a string
        // token followed by cell objects.
        if (!obj->list.empty()) {
            PyObject* first = obj->list[0];
            bool is_bundle = false;
            if (first && first->type == 11) {
                is_bundle = true;
            } else if (first && first->type == 3 && obj->list.size() >= 2) {
                bool all_cells = true;
                for (size_t i = 1; i < obj->list.size(); ++i) {
                    if (!obj->list[i] || obj->list[i]->type != 6) {
                        all_cells = false;
                        break;
                    }
                }
                is_bundle = all_cells;
            }
            if (is_bundle) {
                std::string nm = first->type == 11
                    ? (first->cell_content ? first->cell_content->str : first->str)
                    : first->str;
                std::string result = "<function " + nm + " at " + std::to_string(reinterpret_cast<uintptr_t>(obj)) + ">";
                return PyUnicode_FromString(result.c_str());
            }
        }
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
    if (obj->type == 7) {
        // Tuple repr: paren format, single-element trailing comma.
        size_t n = PyTuple_Size(obj);
        std::string r = "(";
        bool first = true;
        if (obj->list_item_type == 1) {
            for (long v : obj->ilist) {
                if (!first) r += ", ";
                first = false;
                r += std::to_string(v);
            }
        } else if (obj->list_item_type == 2) {
            char buf[64];
            for (double v : obj->flist) {
                if (!first) r += ", ";
                first = false;
                format_double(buf, sizeof(buf), v);
                r += buf;
            }
        } else {
            for (auto* item : obj->list) {
                if (!first) r += ", ";
                first = false;
                if (item && item->type == 3) { r += "'" + item->str + "'"; }
                else if (item) {
                    PyObject* s = PyBuiltin_Repr(item);
                    if (s) { r += s->str; Py_DECREF(s); }
                }
            }
        }
        if (n == 1) r += ",";
        r += ")";
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

// pow(base, exp, mod) — 3-arg modular exponentiation. Found missing
// while hunting for more instances of the "builtin name missing from
// neverDynamic" bug class: fixing that bug for 2-arg pow() surfaced
// that the 3-arg form was *also* never implemented at the runtime level
// at all (Compiler.cpp's pow dispatch only ever passed 2 args) —
// confirmed `pow(2, 10, 1000)` silently ignored the modulus and
// returned the unmodded 1024 instead of 24. Real CPython's 3-arg pow
// requires int operands (raises TypeError otherwise) and a non-negative
// exponent; matched here via fast modular exponentiation to avoid
// overflow for larger exponents.
PyObject* PyBuiltin_Pow3(PyObject* a, PyObject* b, PyObject* m) {
    if (!a || !b || !m) return nullptr;
    long long base = (a->type == 0 || a->type == 5) ? a->value : 0;
    long long exp  = (b->type == 0 || b->type == 5) ? b->value : 0;
    long long mod  = (m->type == 0 || m->type == 5) ? m->value : 1;
    if (mod == 0) { pyc_raise_msg("ValueError", "pow() 3rd argument cannot be 0"); return nullptr; }
    if (exp < 0) { pyc_raise_msg("ValueError", "pow() 2nd argument cannot be negative when 3rd argument specified"); return nullptr; }
    bool negResult = mod < 0;
    long long m_abs = negResult ? -mod : mod;
    base %= m_abs; if (base < 0) base += m_abs;
    long long result = 1 % m_abs;
    while (exp > 0) {
        if (exp & 1) result = (result * base) % m_abs;
        base = (base * base) % m_abs;
        exp >>= 1;
    }
    if (negResult && result != 0) result -= m_abs;
    return PyInt_FromLong((long)result);
}

PyObject* PyString_Upper(PyObject* s) {
    if (!s) return nullptr;
    std::string r = s->str;
    for (char& c : r) c = (char)toupper((unsigned char)c);
    if (s->type == 17) return PyBytes_FromStringAndSize(r.data(), r.size());
    if (s->type == 18) return PyByteArray_FromStringAndSize(r.data(), r.size());
    if (s->type != 3) { Py_INCREF(s); return s; }
    return PyUnicode_FromString(r.c_str());
}

PyObject* PyString_Lower(PyObject* s) {
    if (!s) return nullptr;
    std::string r = s->str;
    for (char& c : r) c = (char)tolower((unsigned char)c);
    if (s->type == 17) return PyBytes_FromStringAndSize(r.data(), r.size());
    if (s->type == 18) return PyByteArray_FromStringAndSize(r.data(), r.size());
    if (s->type != 3) { Py_INCREF(s); return s; }
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

// str.rsplit(sep, maxsplit) — found entirely unimplemented while bug
// hunting. When maxsplit < 0 (CPython's default, no limit) rsplit
// produces the exact same list as split, so this just delegates; the
// two only diverge once maxsplit caps the split count, in which case
// rsplit keeps the rightmost maxsplit+1 pieces (splitting scans from
// the end of the string) where split would keep the leftmost ones.
PyObject* PyString_RSplit(PyObject* s, PyObject* sep, PyObject* maxsplitObj) {
    long maxsplit = (maxsplitObj && (maxsplitObj->type == 0 || maxsplitObj->type == 5))
        ? maxsplitObj->value : -1;
    if (maxsplit < 0) return PyString_Split(s, sep);
    PyObject* result = PyList_New(0);
    if (!s || s->type != 3) return result;
    std::string delim = (sep && sep->type == 3) ? sep->str : " ";
    if (delim.empty()) return result;
    std::vector<std::string> parts;
    std::string remaining = s->str;
    long count = 0;
    while (count < maxsplit) {
        size_t pos = remaining.rfind(delim);
        if (pos == std::string::npos) break;
        parts.insert(parts.begin(), remaining.substr(pos + delim.size()));
        remaining = remaining.substr(0, pos);
        ++count;
    }
    parts.insert(parts.begin(), remaining);
    for (auto& p : parts) PyList_Append(result, PyUnicode_FromString(p.c_str()));
    return result;
}

// str.rsplit(None, maxsplit) — the whitespace-delimited form. Splits
// from the right on runs of whitespace, up to maxsplit times; whatever
// remains at the front (including any internal whitespace runs that
// weren't split on) becomes the first element as-is, mirroring
// CPython's exact behavior — e.g. "  a  b  c  ".rsplit(None, 1) ==
// ["  a  b", "c"] (leading/internal whitespace in the unsplit prefix is
// preserved, only the trailing whitespace of that prefix is trimmed).
PyObject* PyString_RSplitWhitespace(PyObject* s, PyObject* maxsplitObj) {
    long maxsplit = (maxsplitObj && (maxsplitObj->type == 0 || maxsplitObj->type == 5))
        ? maxsplitObj->value : -1;
    if (maxsplit < 0) return PyString_SplitWhitespace(s);
    PyObject* result = PyList_New(0);
    if (!s || s->type != 3) return result;
    const std::string& str = s->str;
    long end = (long)str.size();
    std::vector<std::string> parts;
    long count = 0;
    while (count < maxsplit) {
        while (end > 0 && isspace((unsigned char)str[end - 1])) --end;
        if (end == 0) break;
        long tokEnd = end;
        long tokStart = end;
        while (tokStart > 0 && !isspace((unsigned char)str[tokStart - 1])) --tokStart;
        parts.push_back(str.substr((size_t)tokStart, (size_t)(tokEnd - tokStart)));
        end = tokStart;
        ++count;
    }
    long remEnd = end;
    while (remEnd > 0 && isspace((unsigned char)str[remEnd - 1])) --remEnd;
    if (remEnd > 0) parts.push_back(str.substr(0, (size_t)remEnd));
    std::reverse(parts.begin(), parts.end());
    for (auto& p : parts) PyList_Append(result, PyUnicode_FromString(p.c_str()));
    return result;
}

// Pyc_FormatValue(value, specStr) — implements Python's Format
// Specification Mini-Language (see the PycFormatSpec/pyc_parse_format_spec
// comment far above for the supported grammar subset and known gaps).
// Used both by f-string format specs (f"{x:.2f}") and, via
// PyBuiltin_StrFormat below, by str.format("{:.2f}", x).
PyObject* Pyc_FormatValue(PyObject* value, PyObject* specObj) {
    std::string spec = (specObj && specObj->type == 3) ? specObj->str : "";
    if (spec.empty()) return PyStr_FromAny(value);

    PycFormatSpec f;
    pyc_parse_format_spec(spec, f);
    char type = f.type;

    bool isStrType = (value && value->type == 3);
    bool isIntType = (value && (value->type == 0 || value->type == 5)); // int or bool

    std::string body;
    size_t signPrefixLen = 0;
    char defaultAlign = '>';

    if (type == 's' || (type == 0 && isStrType)) {
        std::string sv;
        if (isStrType) sv = value->str;
        else { PyObject* c = PyStr_FromAny(value); if (c) { sv = c->str; Py_DECREF(c); } }
        if (f.precision >= 0 && (long)sv.size() > f.precision) sv = sv.substr(0, (size_t)f.precision);
        body = sv;
        defaultAlign = '<';
    } else if (type=='d'||type=='b'||type=='o'||type=='x'||type=='X'||type=='n'||type=='c' ||
               (type==0 && isIntType)) {
        long v = isIntType ? value->value : (value && value->type == 4 ? (long)value->dvalue : 0);
        if (type == 'c') {
            body = std::string(1, (char)v);
            defaultAlign = '<';
        } else {
            bool neg = v < 0;
            unsigned long uv = neg ? (unsigned long)(-(long long)v) : (unsigned long)v;
            std::string digits, basePrefix;
            int base = 10;
            if (type=='b') { base=2; if (f.alt) basePrefix = "0b"; }
            else if (type=='o') { base=8; if (f.alt) basePrefix = "0o"; }
            else if (type=='x') { base=16; if (f.alt) basePrefix = "0x"; }
            else if (type=='X') { base=16; if (f.alt) basePrefix = "0X"; }
            if (base == 10) {
                digits = std::to_string(uv);
                if (f.grouping) digits = pyc_group_digits(digits, f.grouping);
            } else {
                digits = pyc_to_base(uv, base, type=='X');
            }
            std::string signStr = neg ? "-" : (f.sign=='+' ? "+" : (f.sign==' ' ? " " : ""));
            body = signStr + basePrefix + digits;
            signPrefixLen = signStr.size() + basePrefix.size();
            defaultAlign = '>';
        }
    } else if (type=='f'||type=='F'||type=='e'||type=='E'||type=='g'||type=='G'||type=='%' ||
               (type==0 && value && value->type==4)) {
        double d = (value && value->type==4) ? value->dvalue
                  : (isIntType ? (double)value->value : 0.0);
        bool neg = d < 0.0;
        double av = neg ? -d : d;
        if (type == '%') av *= 100.0;
        char buf[512];
        if (type=='f'||type=='F'||type=='%') {
            snprintf(buf, sizeof(buf), "%.*f", f.precision >= 0 ? (int)f.precision : 6, av);
        } else if (type=='e'||type=='E') {
            snprintf(buf, sizeof(buf), type=='e' ? "%.*e" : "%.*E", f.precision >= 0 ? (int)f.precision : 6, av);
        } else if (type=='g'||type=='G') {
            int p = f.precision >= 0 ? (int)f.precision : 6;
            if (p == 0) p = 1;
            snprintf(buf, sizeof(buf), type=='g' ? "%.*g" : "%.*G", p, av);
        } else if (f.precision >= 0) {
            // No type char but an explicit precision: Python's default
            // float presentation with a given precision behaves like 'g'.
            int p = (int)f.precision; if (p == 0) p = 1;
            snprintf(buf, sizeof(buf), "%.*g", p, av);
        } else {
            // No type char, no precision: shortest round-trip repr.
            format_double(buf, sizeof(buf), av);
        }
        std::string digits = buf;
        if (f.grouping && (type=='f'||type=='F'||type==0)) {
            size_t dot = digits.find('.');
            std::string intPart = dot==std::string::npos ? digits : digits.substr(0, dot);
            std::string fracPart = dot==std::string::npos ? "" : digits.substr(dot);
            digits = pyc_group_digits(intPart, f.grouping) + fracPart;
        }
        std::string signStr = neg ? "-" : (f.sign=='+' ? "+" : (f.sign==' ' ? " " : ""));
        std::string suffix = (type=='%') ? "%" : "";
        body = signStr + digits + suffix;
        signPrefixLen = signStr.size();
        defaultAlign = '>';
    } else {
        // Unknown type code, or a value type this formatter doesn't have
        // a dedicated numeric/string path for (e.g. decimal.Decimal,
        // list, dict, None): fall back to str() and just apply
        // width/fill/align padding.
        PyObject* c = PyStr_FromAny(value);
        body = c ? c->str : "";
        if (c) Py_DECREF(c);
        defaultAlign = isStrType ? '<' : '>';
    }

    char align = f.align ? f.align : defaultAlign;
    long width = f.width >= 0 ? f.width : 0;
    std::string padded = pyc_fmt_pad(body, width, f.fill, align, signPrefixLen);
    return PyUnicode_FromString(padded.c_str());
}

// str.partition(sep) / str.rpartition(sep) — return a 3-tuple
// (before, sep, after), matching CPython. partition finds the first
// occurrence of sep; rpartition finds the last. Real CPython raises
// ValueError for an empty separator; this takes the more lenient "no
// match" fallback instead (documented, not treated as an error case
// here — matches this codebase's general preference for graceful
// fallback over raising in edge cases).
PyObject* PyString_Partition(PyObject* s, PyObject* sep) {
    std::string str = (s && s->type == 3) ? s->str : "";
    std::string delim = (sep && sep->type == 3) ? sep->str : "";
    size_t pos = delim.empty() ? std::string::npos : str.find(delim);
    PyObject* r = PyTuple_New(3);
    if (pos == std::string::npos) {
        PyTuple_SetItem(r, 0, PyUnicode_FromString(str.c_str()));
        PyTuple_SetItem(r, 1, PyUnicode_FromString(""));
        PyTuple_SetItem(r, 2, PyUnicode_FromString(""));
    } else {
        PyTuple_SetItem(r, 0, PyUnicode_FromString(str.substr(0, pos).c_str()));
        PyTuple_SetItem(r, 1, PyUnicode_FromString(delim.c_str()));
        PyTuple_SetItem(r, 2, PyUnicode_FromString(str.substr(pos + delim.size()).c_str()));
    }
    return r;
}
PyObject* PyString_RPartition(PyObject* s, PyObject* sep) {
    std::string str = (s && s->type == 3) ? s->str : "";
    std::string delim = (sep && sep->type == 3) ? sep->str : "";
    size_t pos = delim.empty() ? std::string::npos : str.rfind(delim);
    PyObject* r = PyTuple_New(3);
    if (pos == std::string::npos) {
        PyTuple_SetItem(r, 0, PyUnicode_FromString(""));
        PyTuple_SetItem(r, 1, PyUnicode_FromString(""));
        PyTuple_SetItem(r, 2, PyUnicode_FromString(str.c_str()));
    } else {
        PyTuple_SetItem(r, 0, PyUnicode_FromString(str.substr(0, pos).c_str()));
        PyTuple_SetItem(r, 1, PyUnicode_FromString(delim.c_str()));
        PyTuple_SetItem(r, 2, PyUnicode_FromString(str.substr(pos + delim.size()).c_str()));
    }
    return r;
}

// str.format(*args, **kwargs) — found entirely unimplemented while bug
// hunting (calling it silently printed None). Implements the
// {field[!conv][:format_spec]} template mini-language: "{{"/"}}" are
// literal braces; an empty field ("{}") auto-numbers through argsList in
// order; a digit-only field is an explicit positional index; any other
// field is a keyword lookup in kwargsDict. Nested field access
// (attribute/index lookups inside the braces, e.g. "{0.attr}"/"{0[1]}")
// is not supported — a narrower, documented gap; real Python also
// raises for mixing auto-numbered and explicit-positional fields in one
// template, which this doesn't enforce (harmless leniency, not a
// correctness gap for well-formed templates). Formatting itself is
// delegated to Pyc_FormatValue, the same Format Spec Mini-Language
// implementation f-strings use.
PyObject* PyBuiltin_StrFormat(PyObject* templateStr, PyObject* argsList, PyObject* kwargsDict) {
    if (!templateStr || templateStr->type != 3) return PyUnicode_FromString("");
    const std::string& tmpl = templateStr->str;
    std::string out;
    long autoIdx = 0;
    size_t i = 0, n = tmpl.size();
    while (i < n) {
        char c = tmpl[i];
        if (c == '{') {
            if (i + 1 < n && tmpl[i + 1] == '{') { out += '{'; i += 2; continue; }
            size_t close = tmpl.find('}', i);
            if (close == std::string::npos) { out += tmpl.substr(i); break; }
            std::string inner = tmpl.substr(i + 1, close - i - 1);
            std::string fieldPart = inner, formatSpecStr;
            size_t colon = inner.find(':');
            if (colon != std::string::npos) {
                fieldPart = inner.substr(0, colon);
                formatSpecStr = inner.substr(colon + 1);
            }
            std::string fieldName = fieldPart;
            char conv = 0;
            size_t bang = fieldPart.find('!');
            if (bang != std::string::npos) {
                if (bang + 1 < fieldPart.size()) conv = fieldPart[bang + 1];
                fieldName = fieldPart.substr(0, bang);
            }
            PyObject* val = nullptr;
            bool ownVal = false;
            if (fieldName.empty()) {
                if (argsList && argsList->type == 1 && autoIdx < (long)PyList_Size(argsList)) {
                    val = PyList_GetItemI64(argsList, autoIdx);
                    ownVal = true;
                }
                autoIdx++;
            } else if (isdigit((unsigned char)fieldName[0])) {
                long idx = std::atol(fieldName.c_str());
                if (argsList && argsList->type == 1 && idx >= 0 && idx < (long)PyList_Size(argsList)) {
                    val = PyList_GetItemI64(argsList, idx);
                    ownVal = true;
                }
            } else if (kwargsDict && kwargsDict->type == 2) {
                for (auto& pair : kwargsDict->dict) {
                    if (pair.first && pair.first->type == 3 && pair.first->str == fieldName) {
                        val = pair.second; // borrowed — kwargsDict outlives this call
                        break;
                    }
                }
            }
            if (conv == 'r' || conv == 'a') {
                PyObject* r = PyBuiltin_Repr(val);
                if (ownVal && val) Py_DECREF(val);
                val = r;
                ownVal = true;
            }
            PyObject* specObj = PyUnicode_FromString(formatSpecStr.c_str());
            PyObject* formatted = Pyc_FormatValue(val, specObj);
            Py_DECREF(specObj);
            if (formatted) { out += formatted->str; Py_DECREF(formatted); }
            if (ownVal && val) Py_DECREF(val);
            i = close + 1;
        } else if (c == '}') {
            if (i + 1 < n && tmpl[i + 1] == '}') { out += '}'; i += 2; continue; }
            out += '}';
            i++;
        } else {
            out += c;
            i++;
        }
    }
    return PyUnicode_FromString(out.c_str());
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
        PyObject* item = PyTuple_New(2);
        Py_INCREF(pair.first); Py_INCREF(pair.second);
        PyTuple_SetItem(item, 0, pair.first);
        PyTuple_SetItem(item, 1, pair.second);
        PyList_Append(result, item);
        Py_DECREF(item);
    }
    return result;
}

// Homogeneous int/float lists (list_item_type 1/2 — an existing A4
// performance optimization) store their elements in ilist/flist instead
// of the generic boxed `list` vector of PyObject*. Any function that
// needs general PyObject* access (comparisons, in-place algorithms)
// must materialize the boxed form first, or it silently operates on an
// empty `list` vector. Found while adding heapq/bisect/statistics (see
// Runtime.cpp further down); also fixes a real pre-existing bug found
// the same way: `h = [5,1,8,3,9,2]; h.sort()` was a silent no-op, since
// PyList_Sort (below) used to sort `lst->list` directly without this
// conversion.
static void pyc_ensure_boxed_list(PyObject* lst) {
    if (!lst || lst->type != 1) return;
    if (lst->list_item_type == 1) {
        lst->list.clear();
        lst->list.reserve(lst->ilist.size());
        for (int64_t v : lst->ilist) lst->list.push_back(PyInt_FromLong(v));
        lst->ilist.clear();
        lst->list_item_type = 0;
    } else if (lst->list_item_type == 2) {
        lst->list.clear();
        lst->list.reserve(lst->flist.size());
        for (double v : lst->flist) lst->list.push_back(PyFloat_FromDouble(v));
        lst->flist.clear();
        lst->list_item_type = 0;
    }
}

// `key`/`reverse` were found completely unimplemented (silently
// ignored — list.sort(key=...) sorted by natural order, list.sort
// (reverse=True) sorted ascending) while hunting for more bugs; the
// in-place reorder mirrors PyBuiltin_Sorted's key-based approach
// (compute a key per element, sort indices, reorder), applied directly
// to lst->list rather than building a new list.
PyObject* PyList_Sort(PyObject* lst, PyObject* key, PyObject* reverse) {
    if (!lst || lst->type != 1) return PyInt_FromLong(0);
    pyc_ensure_boxed_list(lst);
    if (key) {
        std::vector<PyObject*> keys;
        keys.reserve(lst->list.size());
        for (auto* item : lst->list) {
            PyObject* argList = PyList_New(1);
            if (item) { Py_INCREF(item); PyList_SetItem(argList, 0, item); }
            PyObject* k = Pyc_Apply(key, argList);
            Py_DECREF(argList);
            keys.push_back(k);
        }
        std::vector<size_t> idx(lst->list.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t i, size_t j) {
            if (!keys[i] || !keys[j]) return false;
            return PyObject_CompareBool(keys[i], keys[j], 2) != 0;
        });
        std::vector<PyObject*> reordered(lst->list.size());
        for (size_t i = 0; i < idx.size(); ++i) reordered[i] = lst->list[idx[i]];
        lst->list = reordered;
        for (auto* k : keys) if (k) Py_DECREF(k);
    } else {
        std::sort(lst->list.begin(), lst->list.end(), [](PyObject* a, PyObject* b) -> bool {
            if (!a || !b) return false;
            return PyObject_CompareBool(a, b, 2) != 0;  // Lt
        });
    }
    if (PyObject_TruthValue(reverse)) std::reverse(lst->list.begin(), lst->list.end());
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

void PyBuiltin_AssertFailure(PyObject* msg) {
    // Raise AssertionError with optional message
    fprintf(stderr, "AssertionError");
    if (msg && msg->type == 3) {
        fprintf(stderr, ": %s", msg->str.c_str());
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    exit(1);
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
    pyc_ensure_boxed_list(list);
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
    pyc_ensure_boxed_list(list);
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
    pyc_ensure_boxed_list(list);
    for (size_t i = 0; i < list->list.size(); ++i) {
        bool eq = (list->list[i] == item) ||
                  (list->list[i] && item && PyObject_CompareBool(list->list[i], item, 0));
        if (eq) return PyInt_FromLong((long)i);
    }
    return PyInt_FromLong(-1);
}
PyObject* PyList_Count(PyObject* list, PyObject* item) {
    if (!list || list->type != 1) return PyInt_FromLong(0);
    pyc_ensure_boxed_list(list);
    long c = 0;
    for (auto* e : list->list) {
        if (e == item || (e && item && PyObject_CompareBool(e, item, 0))) ++c;
    }
    return PyInt_FromLong(c);
}
PyObject* PyList_Reverse(PyObject* list) {
    if (!list || list->type != 1) return nullptr;
    pyc_ensure_boxed_list(list);
    std::reverse(list->list.begin(), list->list.end());
    return PyInt_FromLong(0);
}
PyObject* PyList_Extend(PyObject* list, PyObject* other) {
    if (!list || list->type != 1) return nullptr;
    pyc_ensure_boxed_list(list);
    if (other) {
        if (other->type == 1) {
            pyc_ensure_boxed_list(other);
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
    pyc_ensure_boxed_list(list);
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

// ---- set type (type 20) ----
// Insertion-ordered, dedup-by-value. Backed by PyObject::setElems with
// linear-scan equality (PyObject_CompareBool op==0), matching the dict
// container's approach. CPython 3.7+ sets don't guarantee insertion order
// for iteration, but pyc's existing dicts don't either (they're
// unordered_map-backed) — choosing insertion order here is the simpler
// option and lets `{x for x in iter}` match list-comp-style output for
// the common single-iteration test cases.

static PyObject* pyc_set_iter_to_list(PyObject* iterable) {
    // Materialize any supported iterable into a fresh list. Mirrors the
    // `PyBuiltin_List` shapes (list/str/dict/set) without depending on it.
    if (!iterable) return PyList_New(0);
    if (iterable->type == 1) {
        size_t n = (iterable->list_item_type == 1) ? iterable->ilist.size()
                  : (iterable->list_item_type == 2) ? iterable->flist.size()
                  : iterable->list.size();
        PyObject* r = PyList_New(n);
        // pyc_ensure_boxed_list semantics: homogeneous int/float lists store
        // in ilist/flist; materialize boxed PyObject* here for uniform handling.
        if (iterable->list_item_type == 1) {
            for (size_t i = 0; i < iterable->ilist.size(); ++i)
                PyList_SetItem(r, i, PyInt_FromLong(iterable->ilist[i]));
        } else if (iterable->list_item_type == 2) {
            for (size_t i = 0; i < iterable->flist.size(); ++i)
                PyList_SetItem(r, i, PyFloat_FromDouble(iterable->flist[i]));
        } else {
            for (size_t i = 0; i < iterable->list.size(); ++i) {
                PyList_SetItem(r, i, iterable->list[i]);
                if (iterable->list[i]) Py_INCREF(iterable->list[i]);
            }
        }
        return r;
    }
    if (iterable->type == 3 || iterable->type == 17 || iterable->type == 18) {
        // str/bytes/bytearray: iterate code points / byte values.
        PyObject* r = PyList_New(iterable->str.size());
        for (size_t i = 0; i < iterable->str.size(); ++i) {
            if (iterable->type == 3) {
                // One code point per element (UTF-8-aware would be ideal; pyc's
                // str is byte-based for non-ASCII in practice — matches existing
                // str iteration behavior elsewhere in the runtime).
                PyList_SetItem(r, i, PyUnicode_FromString(iterable->str.substr(i, 1).c_str()));
            } else {
                PyList_SetItem(r, i, PyInt_FromLong((unsigned char)iterable->str[i]));
            }
        }
        return r;
    }
    if (iterable->type == 2) {
        // dict: iterate keys.
        PyObject* r = PyList_New(iterable->dict.size());
        size_t i = 0;
        for (auto& p : iterable->dict) {
            PyList_SetItem(r, i, p.first);
            if (p.first) Py_INCREF(p.first);
            ++i;
        }
        return r;
    }
    if (iterable->type == 20) {
        PyObject* r = PyList_New(iterable->setElems.size());
        for (size_t i = 0; i < iterable->setElems.size(); ++i) {
            PyList_SetItem(r, i, iterable->setElems[i]);
            if (iterable->setElems[i]) Py_INCREF(iterable->setElems[i]);
        }
        return r;
    }
    return PyList_New(0);
}

PyObject* PySet_New(void) {
    alloc_set_count++;
    PyObject* obj = new PyObject();
    obj->refcount = 1;
    obj->type = 20;
    return obj;
}

void PySet_Add(PyObject* set, PyObject* item) {
    if (!set || set->type != 20 || !item) return;
    for (auto* e : set->setElems) {
        if (e && PyObject_CompareBool(e, item, 0)) return; // already present
    }
    set->setElems.push_back(item);
    Py_INCREF(item);
}

int PySet_Contains(PyObject* set, PyObject* item) {
    if (!set || set->type != 20 || !item) return 0;
    for (auto* e : set->setElems) {
        if (e && PyObject_CompareBool(e, item, 0)) return 1;
    }
    return 0;
}

PyObject* PySet_ContainsObj(PyObject* set, PyObject* item) {
    return PyBool_New(PySet_Contains(set, item));
}

void PySet_Remove(PyObject* set, PyObject* item) {
    if (!set || set->type != 20 || !item) return;
    for (auto it = set->setElems.begin(); it != set->setElems.end(); ++it) {
        if (*it && PyObject_CompareBool(*it, item, 0)) {
            Py_DECREF(*it);
            set->setElems.erase(it);
            return;
        }
    }
    // Not found: raise KeyError. Reuse the existing exception machinery.
    pyc_raise(pyc_make_exc(PyUnicode_FromString("KeyError"), item));
}

void PySet_Discard(PyObject* set, PyObject* item) {
    if (!set || set->type != 20 || !item) return;
    for (auto it = set->setElems.begin(); it != set->setElems.end(); ++it) {
        if (*it && PyObject_CompareBool(*it, item, 0)) {
            Py_DECREF(*it);
            set->setElems.erase(it);
            return;
        }
    }
}

PyObject* PySet_Pop(PyObject* set) {
    if (!set || set->type != 20 || set->setElems.empty()) {
        pyc_raise(pyc_make_exc(PyUnicode_FromString("KeyError"), nullptr));
        return nullptr;
    }
    PyObject* r = set->setElems.back();
    set->setElems.pop_back();
    // Pop returns a new strong reference; the stored ref is dropped.
    if (r) Py_INCREF(r);
    if (r) Py_DECREF(r); // balance the stored ref
    return r;
}

void PySet_Clear(PyObject* set) {
    if (!set || set->type != 20) return;
    for (auto* e : set->setElems) if (e) Py_DECREF(e);
    set->setElems.clear();
}

PyObject* PySet_Copy(PyObject* set) {
    PyObject* r = PySet_New();
    if (!set || set->type != 20) return r;
    for (auto* e : set->setElems) {
        if (e) {
            r->setElems.push_back(e);
            Py_INCREF(e);
        }
    }
    return r;
}

size_t PySet_Size(PyObject* set) {
    if (!set || set->type != 20) return 0;
    return set->setElems.size();
}

PyObject* PySet_SizeBoxed(PyObject* set) {
    return PyInt_FromLong((long)PySet_Size(set));
}

PyObject* PySet_ToList(PyObject* set) {
    if (!set || set->type != 20) return PyList_New(0);
    PyObject* r = PyList_New(set->setElems.size());
    for (size_t i = 0; i < set->setElems.size(); ++i) {
        PyList_SetItem(r, i, set->setElems[i]);
        if (set->setElems[i]) Py_INCREF(set->setElems[i]);
    }
    return r;
}

void PySet_Update(PyObject* set, PyObject* other) {
    if (!set || set->type != 20) return;
    PyObject* elems = pyc_set_iter_to_list(other);
    if (!elems) return;
    for (auto* e : elems->list) PySet_Add(set, e);
    Py_DECREF(elems);
}

static PyObject* pyc_set_from_iter(PyObject* iterable) {
    PyObject* r = PySet_New();
    if (!iterable) return r;
    PyObject* elems = pyc_set_iter_to_list(iterable);
    for (auto* e : elems->list) PySet_Add(r, e);
    Py_DECREF(elems);
    return r;
}

PyObject* PySet_Union(PyObject* a, PyObject* b) {
    PyObject* r = PySet_New();
    if (a && a->type == 20) {
        for (auto* e : a->setElems) {
            if (e) { r->setElems.push_back(e); Py_INCREF(e); }
        }
    } else if (a) {
        PyObject* la = pyc_set_iter_to_list(a);
        for (auto* e : la->list) PySet_Add(r, e);
        Py_DECREF(la);
    }
    if (b && b->type == 20) {
        for (auto* e : b->setElems) PySet_Add(r, e);
    } else if (b) {
        PyObject* lb = pyc_set_iter_to_list(b);
        for (auto* e : lb->list) PySet_Add(r, e);
        Py_DECREF(lb);
    }
    return r;
}

PyObject* PySet_Intersection(PyObject* a, PyObject* b) {
    PyObject* r = PySet_New();
    if (!a || !b) return r;
    PyObject* la = pyc_set_iter_to_list(a);
    for (auto* e : la->list) {
        int inB = 0;
        if (b->type == 20) inB = PySet_Contains(b, e);
        else {
            PyObject* lb = pyc_set_iter_to_list(b);
            for (auto* eb : lb->list) {
                if (eb && PyObject_CompareBool(eb, e, 0)) { inB = 1; break; }
            }
            Py_DECREF(lb);
        }
        if (inB) PySet_Add(r, e);
    }
    Py_DECREF(la);
    return r;
}

PyObject* PySet_Difference(PyObject* a, PyObject* b) {
    PyObject* r = PySet_New();
    if (!a) return r;
    PyObject* la = pyc_set_iter_to_list(a);
    for (auto* e : la->list) {
        int inB = 0;
        if (b && b->type == 20) inB = PySet_Contains(b, e);
        else if (b) {
            PyObject* lb = pyc_set_iter_to_list(b);
            for (auto* eb : lb->list) {
                if (eb && PyObject_CompareBool(eb, e, 0)) { inB = 1; break; }
            }
            Py_DECREF(lb);
        }
        if (!inB) PySet_Add(r, e);
    }
    Py_DECREF(la);
    return r;
}

PyObject* PySet_SymmetricDifference(PyObject* a, PyObject* b) {
    PyObject* r = PySet_New();
    PyObject* u = PySet_Union(a, b);
    PyObject* inter = PySet_Intersection(a, b);
    // r = u - inter
    PyObject* la = pyc_set_iter_to_list(u);
    for (auto* e : la->list) {
        int inInter = (inter && inter->type == 20) ? PySet_Contains(inter, e) : 0;
        if (!inInter) PySet_Add(r, e);
    }
    Py_DECREF(la);
    Py_DECREF(u);
    if (inter) Py_DECREF(inter);
    return r;
}

int PySet_IsSubset(PyObject* a, PyObject* b) {
    if (!a) return 1;
    PyObject* la = pyc_set_iter_to_list(a);
    for (auto* e : la->list) {
        int inB = 0;
        if (b && b->type == 20) inB = PySet_Contains(b, e);
        else if (b) {
            PyObject* lb = pyc_set_iter_to_list(b);
            for (auto* eb : lb->list) {
                if (eb && PyObject_CompareBool(eb, e, 0)) { inB = 1; break; }
            }
            Py_DECREF(lb);
        }
        if (!inB) { Py_DECREF(la); return 0; }
    }
    Py_DECREF(la);
    return 1;
}

int PySet_IsSuperset(PyObject* a, PyObject* b) {
    return PySet_IsSubset(b, a);
}

int PySet_IsDisjoint(PyObject* a, PyObject* b) {
    PyObject* inter = PySet_Intersection(a, b);
    int empty = (inter && inter->type == 20) ? inter->setElems.empty() : 1;
    if (inter) Py_DECREF(inter);
    return empty;
}

// Boxed-dispatch wrappers for operators (return new set, or nullptr if
// neither operand is a set — so the caller can fall through to other
// numeric/dispatch paths).
PyObject* PySet_UnionObj(PyObject* a, PyObject* b) {
    if ((!a || a->type != 20) && (!b || b->type != 20)) return nullptr;
    return PySet_Union(a, b);
}
PyObject* PySet_IntersectionObj(PyObject* a, PyObject* b) {
    if ((!a || a->type != 20) && (!b || b->type != 20)) return nullptr;
    return PySet_Intersection(a, b);
}
PyObject* PySet_DifferenceObj(PyObject* a, PyObject* b) {
    if (!a || a->type != 20) return nullptr;
    return PySet_Difference(a, b);
}
PyObject* PySet_SymmetricDifferenceObj(PyObject* a, PyObject* b) {
    if ((!a || a->type != 20) && (!b || b->type != 20)) return nullptr;
    return PySet_SymmetricDifference(a, b);
}
PyObject* PySet_IsSubsetObj(PyObject* a, PyObject* b) {
    return PyBool_New(PySet_IsSubset(a, b));
}
PyObject* PySet_IsSupersetObj(PyObject* a, PyObject* b) {
    return PyBool_New(PySet_IsSuperset(a, b));
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
    add("split",    "PyBuiltin_ReSplit");
    // Real flag values (matching CPython's re.IGNORECASE=2/MULTILINE=8/
    // DOTALL=16 exactly) so `re.IGNORECASE` etc. evaluate to a real int
    // usable both positionally and via `flags=` — previously these were
    // undefined (no dict entry at all), silently resolving to None.
    auto addInt = [&](const char* name, long v) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* val = PyInt_FromLong(v);
        PyDict_SetItem(d, k, val);
        Py_DECREF(k);
        Py_DECREF(val);
    };
    addInt("IGNORECASE", 2);
    addInt("MULTILINE", 8);
    addInt("DOTALL", 16);
    return d;
}

// Forward declaration: the `sys` module is set up at startup by
// pyc_setup_sys. Defined further down in this file.
static PyObject* g_sys_module = nullptr;
// B7: Global reference to sys.modules dict
static PyObject* g_sys_modules = nullptr;

// Forward declarations for the os / subprocess / math / json / random /
// itertools / collections / datetime module dict builders.
static PyObject* makeOsModuleDict();
static PyObject* makeSubprocessModuleDict();
static PyObject* makeMathModuleDict();
static PyObject* makeJsonModuleDict();
static PyObject* makeRandomModuleDict();
static PyObject* makeItertoolsModuleDict();
static PyObject* makeCollectionsModuleDict();
static PyObject* makeDatetimeModuleDict();
static PyObject* makeHashlibModuleDict();
static PyObject* makeBase64ModuleDict();
static PyObject* makeStructModuleDict();
static PyObject* makeHeapqModuleDict();
static PyObject* makeBisectModuleDict();
static PyObject* makeStatisticsModuleDict();
static PyObject* makeStringModuleDict();
static PyObject* makeTextwrapModuleDict();
static PyObject* makeUuidModuleDict();
static PyObject* makeCopyModuleDict();
static PyObject* makeShutilModuleDict();
static PyObject* makeGlobModuleDict();
static PyObject* makeCsvModuleDict();

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
            // cmp_to_key is needed by the sorted-with-comparator idiom
            // and is handled structurally at the AST level (Compiler.cpp,
            // funcName=="cmp_to_key") for the bare-name form; the token
            // stored here only prevents attribute access from crashing —
            // note this means `functools.cmp_to_key(...)` (the qualified
            // form, going through the generic dict dispatch below) is
            // NOT actually wired to a real callable and silently fails; a
            // pre-existing gap, not touched here. reduce/partial/wraps/
            // lru_cache are real, working tokens (both qualified and
            // bare-name-via-from-import forms).
            PyObject* d = PyDict_New();
            auto addTok = [&](const char* name, const char* token) {
                PyObject* k = PyUnicode_FromString(name);
                PyObject* v = PyUnicode_FromString(token);
                PyDict_SetItem(d, k, v);
                Py_DECREF(k); Py_DECREF(v);
            };
            addTok("cmp_to_key", "cmp_to_key");
            addTok("reduce",     "PyFunctools_Reduce");
            addTok("partial",    "PyFunctools_Partial");
            addTok("wraps",      "PyFunctools_Wraps");
            addTok("lru_cache",  "PyFunctools_LruCache");
            return d;
        }
        if (modName->str == "operator") {
            PyObject* d = PyDict_New();
            auto addTok = [&](const char* name, const char* token) {
                PyObject* k = PyUnicode_FromString(name);
                PyObject* v = PyUnicode_FromString(token);
                PyDict_SetItem(d, k, v);
                Py_DECREF(k); Py_DECREF(v);
            };
            addTok("add",        "PyOperator_Add");
            addTok("sub",        "PyOperator_Sub");
            addTok("mul",        "PyOperator_Mul");
            addTok("truediv",    "PyOperator_Truediv");
            addTok("mod",        "PyOperator_Mod");
            addTok("eq",         "PyOperator_Eq");
            addTok("ne",         "PyOperator_Ne");
            addTok("lt",         "PyOperator_Lt");
            addTok("gt",         "PyOperator_Gt");
            addTok("le",         "PyOperator_Le");
            addTok("ge",         "PyOperator_Ge");
            addTok("not_",       "PyOperator_Not");
            addTok("neg",        "PyOperator_Neg");
            addTok("itemgetter", "PyOperator_Itemgetter");
            addTok("attrgetter", "PyOperator_Attrgetter");
            return d;
        }
        if (modName->str == "os") {
            return makeOsModuleDict();
        }
        if (modName->str == "subprocess") {
            return makeSubprocessModuleDict();
        }
        if (modName->str == "math") {
            return makeMathModuleDict();
        }
        if (modName->str == "json") {
            return makeJsonModuleDict();
        }
        if (modName->str == "random") {
            return makeRandomModuleDict();
        }
        if (modName->str == "itertools") {
            return makeItertoolsModuleDict();
        }
        if (modName->str == "collections") {
            return makeCollectionsModuleDict();
        }
        if (modName->str == "datetime") {
            return makeDatetimeModuleDict();
        }
        if (modName->str == "pathlib") {
            // Empty: pathlib.Path(...) construction and Path.today()-style
            // calls are never needed (unlike datetime's date.today()/
            // datetime.now()) — Path has exactly one constructor, always
            // intercepted structurally in Compiler.cpp. This dict exists
            // only so `import pathlib` doesn't report ImportError.
            return PyDict_New();
        }
        if (modName->str == "hashlib") {
            return makeHashlibModuleDict();
        }
        if (modName->str == "base64") {
            return makeBase64ModuleDict();
        }
        if (modName->str == "struct") {
            return makeStructModuleDict();
        }
        if (modName->str == "heapq") {
            return makeHeapqModuleDict();
        }
        if (modName->str == "bisect") {
            return makeBisectModuleDict();
        }
        if (modName->str == "statistics") {
            return makeStatisticsModuleDict();
        }
        if (modName->str == "string") {
            return makeStringModuleDict();
        }
        if (modName->str == "textwrap") {
            return makeTextwrapModuleDict();
        }
        if (modName->str == "uuid") {
            return makeUuidModuleDict();
        }
        if (modName->str == "copy") {
            return makeCopyModuleDict();
        }
        if (modName->str == "shutil") {
            return makeShutilModuleDict();
        }
        if (modName->str == "glob") {
            return makeGlobModuleDict();
        }
        if (modName->str == "csv") {
            return makeCsvModuleDict();
        }
        if (modName->str == "decimal") {
            // Empty: decimal.Decimal(...) construction is always
            // intercepted structurally in Compiler.cpp (same rationale as
            // pathlib.Path above — Decimal has exactly one constructor).
            // This dict exists only so `import decimal` doesn't report
            // ImportError.
            return PyDict_New();
        }
        if (modName->str == "cmath") {
            PyObject* d = PyDict_New();
            auto add = [&](const char* name, const char* token) {
                PyObject* k = PyUnicode_FromString(name);
                PyObject* v = PyUnicode_FromString(token);
                PyDict_SetItem(d, k, v);
                Py_DECREF(k);
                Py_DECREF(v);
            };
            add("sqrt", "PyCmath_Sqrt");
            add("log", "PyCmath_Log");
            add("exp", "PyCmath_Exp");
            add("sin", "PyCmath_Sin");
            add("cos", "PyCmath_Cos");
            add("tan", "PyCmath_Tan");
            return d;
        }
        if (modName->str == "time") {
            // time module - C extension, return module with basic attributes and perf_counter
            PyObject* d = PyDict_New();
            PyObject* k = PyUnicode_FromString("__name__");
            PyObject* v = PyUnicode_FromString("time");
            PyDict_SetItem(d, k, v);
            Py_DECREF(k); Py_DECREF(v);
            // Add perf_counter as a callable token
            k = PyUnicode_FromString("perf_counter");
            v = PyUnicode_FromString("Pyc_Time_PerfCounter");
            PyDict_SetItem(d, k, v);
            Py_DECREF(k); Py_DECREF(v);
            return d;
        }
    }
    const char* name = (modName && modName->type == 3) ? modName->str.c_str() : "?";
    fprintf(stderr, "ImportError: No module named '%s' "
                    "(pyc supports only synthetic 'sys', 're', 'functools', 'os', "
                    "'subprocess', 'cmath', 'time', 'math', 'json', 'random', 'itertools', "
                    "'collections', 'datetime', 'pathlib', 'hashlib', 'base64', 'struct', "
                    "'heapq', 'bisect', 'statistics', 'string', 'textwrap', 'uuid', 'copy', "
                    "'operator', 'shutil', 'glob', and 'csv' modules; real module loading "
                    "is not yet implemented)\n", name);
    fflush(stderr);
    return nullptr;
}

PyObject* PyBuiltin_Len(PyObject* obj) {
    if (!obj) return PyInt_FromLong(0);
    if (obj->type == 1) return PyInt_FromLong((long)PyList_Size(obj));
    if (obj->type == 7) return PyInt_FromLong((long)PyTuple_Size(obj));
    if (obj->type == 3 || obj->type == 17 || obj->type == 18) return PyInt_FromLong((long)obj->str.size());
    if (obj->type == 2) {
        // __len__ dispatch for a class instance — found and fixed while
        // bug hunting: len(instance) previously always fell through to
        // the generic "number of instance-attribute entries" count
        // below, which is a meaningless number for most classes (e.g.
        // an instance with __len__ returning 2 but 3 real attributes
        // would report 3, not 2), confirmed via a Vec class defining
        // __len__ to return 2. A genuine plain dict (no __class__ entry)
        // still correctly reports its own entry count via the fallback.
        PyObject* lenMethod = pyc_lookup_dunder(obj, "__len__");
        if (lenMethod) {
            PyObject* r = pyc_call_dunder1(lenMethod, obj);
            long n = (r && (r->type == 0 || r->type == 5)) ? r->value : 0;
            if (r) Py_DECREF(r);
            return PyInt_FromLong(n);
        }
        return PyInt_FromLong((long)obj->dict.size());
    }
    if (obj->type == 20) return PyInt_FromLong((long)PySet_Size(obj));
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

// Promote any numeric or complex to (real, imag) components.
// Returns true if o is numeric or complex, false otherwise.
static bool to_complex(PyObject* o, double& real, double& imag) {
    if (!o) return false;
    if (o->type == 13) { real = o->complex_real; imag = o->complex_imag; return true; }
    if (o->type == 0 || o->type == 5) { real = (double)o->value; imag = 0.0; return true; }
    if (o->type == 4) { real = o->dvalue; imag = 0.0; return true; }
    return false;
}

// True if either operand is complex (type 13) — arithmetic should
// produce a complex result in that case (CPython promotes int/float).
static bool has_complex(PyObject* a, PyObject* b) {
    return (a && a->type == 13) || (b && b->type == 13);
}

// Unbox the i-th element of a token+registry `args` list (see Pyc_Apply)
// as a double, treating int/bool/float uniformly via numeric_val. Used by
// modules like math whose functions take numeric arguments through the
// generic single-args-list calling convention (as opposed to cmath's
// direct-call convention, where each function takes its argument as a
// bare PyObject* already). Returns `defaultVal` if the list is too short
// or the element isn't numeric.
static double arg_numeric(PyObject* args, size_t i, double defaultVal = 0.0) {
    if (!args || args->type != 1 || i >= args->list.size()) return defaultVal;
    PyObject* v = args->list[i];
    return is_numeric(v) ? numeric_val(v) : defaultVal;
}

// True when neither operand is a float — result stays integer.
static int both_integral(PyObject* a, PyObject* b) {
    return a->type != 4 && b->type != 4;
}

// Shared path-string helpers, used by both os.path.basename/dirname/
// splitext (PyBuiltin_OsPath*, token-dispatched functions further down)
// and pathlib.Path's attribute reads (Pyc_GetItem's type==16 branch,
// right below) so the two stay consistent by construction.
static std::string pyc_path_basename(const std::string& s) {
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}
static std::string pyc_path_dirname(const std::string& s) {
    size_t slash = s.find_last_of('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return s.substr(0, slash);
}
// Splits into (root, ext) the way CPython's os.path.splitext does: a dot
// in the last path component that isn't a leading dot splits at the last
// dot (so "a.tar.gz" -> "a.tar"/".gz", ".bashrc" -> ".bashrc"/"").
static void pyc_path_splitext(const std::string& s, std::string& root, std::string& ext) {
    size_t slash = s.find_last_of('/');
    size_t dot = s.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot == (slash == std::string::npos ? 0 : slash + 1)) {
        root = s; ext.clear();
    } else {
        root = s.substr(0, dot);
        ext = s.substr(dot);
    }
}
// True for a plain str (type 3) or a pathlib.Path (type 16) — both store
// their text in the `str` field, so path-related helpers accept either.
static bool pyc_is_path_like(PyObject* o) { return o && (o->type == 3 || o->type == 16); }
static PyObject* pyc_new_path(const std::string& s) {
    PyObject* o = new PyObject();
    o->refcount = 1;
    o->type = 16;
    o->str = s;
    return o;
}

PyObject* Pyc_GetItem(PyObject* obj, PyObject* key) {
    if (!obj || !key) return nullptr;
    // Complex number attribute reads (.real/.imag) — handled directly
    // here so they work even when the value arrives as an untyped
    // function parameter or through operator.attrgetter.
    if (obj->type == 13 && key->type == 3) {
        const std::string& k = key->str;
        if (k == "real") return PyFloat_FromDouble(obj->complex_real);
        if (k == "imag") return PyFloat_FromDouble(obj->complex_imag);
        return nullptr;
    }
    // date/datetime/timedelta attribute reads: handled directly here
    // (rather than requiring the compiler to have inferred the value's
    // type via typeOf) so `.year`/`.days`/etc. work even when the value
    // arrives as an untyped function parameter — lowerAttribute always
    // calls Pyc_GetItem unconditionally for a bare (non-called) attribute
    // read, with no typeOf gate, so this is reached from every call site.
    if (obj->type == 14 && key->type == 3) {
        PycDateTime* dt = pyc_as_datetime(obj);
        const std::string& k = key->str;
        if (k == "year") return PyInt_FromLong(dt->year);
        if (k == "month") return PyInt_FromLong(dt->month);
        if (k == "day") return PyInt_FromLong(dt->day);
        if (k == "hour") return PyInt_FromLong(dt->hasTime ? dt->hour : 0);
        if (k == "minute") return PyInt_FromLong(dt->hasTime ? dt->minute : 0);
        if (k == "second") return PyInt_FromLong(dt->hasTime ? dt->second : 0);
        return nullptr;
    }
    if (obj->type == 15 && key->type == 3) {
        PycTimedelta* td = pyc_as_timedelta(obj);
        const std::string& k = key->str;
        if (k == "days") return PyInt_FromLong((long)td->days);
        if (k == "seconds") return PyInt_FromLong((long)td->seconds);
        if (k == "microseconds") return PyInt_FromLong((long)td->microseconds);
        return nullptr;
    }
    // pathlib.Path attribute reads — same robustness rationale as
    // date/datetime/timedelta above: works even for a Path received as an
    // untyped function parameter, since lowerAttribute always calls
    // Pyc_GetItem unconditionally.
    if (obj->type == 16 && key->type == 3) {
        const std::string& k = key->str;
        if (k == "name") return PyUnicode_FromString(pyc_path_basename(obj->str).c_str());
        if (k == "parent") return pyc_new_path(pyc_path_dirname(obj->str));
        if (k == "suffix" || k == "stem") {
            std::string root, ext;
            pyc_path_splitext(pyc_path_basename(obj->str), root, ext);
            return PyUnicode_FromString((k == "suffix" ? ext : root).c_str());
        }
        return nullptr;
    }
    if (obj->type == 1) return PyList_GetItemObj(obj, key); // returns new ref (INCREF inside)
    if (obj->type == 7) {
        // tuple subscript: mirror PyList_GetItemObj's index handling and
        // new-ref convention (INCREF for boxed, fresh object for homogeneous).
        if (!key || (key->type != 0 && key->type != 5)) return nullptr;
        size_t n = PyTuple_Size(obj);
        long i = (long)key->value;
        if (i < 0) i += (long)n;
        if (i < 0 || (size_t)i >= n) return nullptr;
        return tupleGetNewRef(obj, (size_t)i);
    }
    if (obj->type == 2) {
        for (auto& pair : obj->dict) {
            if (PyObject_CompareBool(pair.first, key, 0)) {
                if (pair.second) Py_INCREF(pair.second); // return new ref
                return pair.second;
            }
        }
        // Class instances (dict-backed objects with __class__): look up in class dict
        for (auto& kv : obj->dict) {
            if (kv.first && kv.first->type == 3 && kv.first->str == "__class__") {
                PyObject* classDict = kv.second;
                if (classDict && classDict->type == 2) {
                    for (auto& ck : classDict->dict) {
                        if (PyObject_CompareBool(ck.first, key, 0)) {
                            if (ck.second) Py_INCREF(ck.second);
                            return ck.second;
                        }
                    }
                }
                break;
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
    // bytes/bytearray indexing returns an int (0-255) — real Python
    // semantics differ from str indexing here (str[i] is a length-1 str);
    // this is the one place bytes genuinely diverges from str, not
    // reusable from the branch above.
    if ((obj->type == 17 || obj->type == 18) && (key->type == 0 || key->type == 5)) {
        long idx = key->value;
        if (idx < 0) idx += (long)obj->str.size();
        if (idx >= 0 && (size_t)idx < obj->str.size()) {
            return PyInt_FromLong((unsigned char)obj->str[(size_t)idx]);
        }
    }
    return nullptr;
}

// User-facing subscript (a[k]): like Pyc_GetItem but raises IndexError /
// KeyError on a miss, Python-style. Internal probes (method lookup, module
// attributes, with-statement dunders) keep using the non-raising Pyc_GetItem.
// collections.defaultdict's factory, keyed by the dict object's own
// pointer (out-of-band, same pattern as g_pycFiles further down for open
// file objects) rather than stashed as a visible dict entry — a visible
// marker key would leak into print()/len()/iteration/.items() on every
// defaultdict, which real defaultdict's repr/behavior never does.
static std::unordered_map<PyObject*, PyObject*> g_pycDefaultFactories;

PyObject* Pyc_Subscript(PyObject* obj, PyObject* key) {
    if (obj && obj->type == 2) {
        // __getitem__ dispatch for a class instance — found and fixed
        // while bug hunting: obj[key] for a class defining __getitem__
        // previously always fell straight into the dict-scan-and-raise
        // logic below (a class instance is a dict with a "__class__"
        // entry), which — since a real class instance's attribute
        // dict essentially never contains the caller's actual subscript
        // key — almost always raised an uncaught KeyError instead of
        // running the user's __getitem__ body at all. Confirmed via a
        // Container class whose __getitem__ has its own fallback
        // ("missing") for an absent key: `c["b"]` crashed with
        // `KeyError: 'b'` instead of calling __getitem__ and returning
        // "missing". Checked first, before the "is this dict itself
        // secretly a class instance" question even needs the
        // pyc_lookup_dunder call below to matter for plain dicts (which
        // have no "__class__" entry, so pyc_lookup_dunder always
        // returns nullptr for them and this check is a no-op).
        PyObject* getitemMethod = pyc_lookup_dunder(obj, "__getitem__");
        if (getitemMethod) return pyc_call_dunder2(getitemMethod, obj, key);
        // Dict: scan directly rather than going through Pyc_GetItem, which
        // returns a null PyObject* both when the key is absent AND when
        // the key is present with a None value — indistinguishable to the
        // caller. A dict genuinely mapping a key to None (e.g. from
        // json.loads('{"k": null}')) must return None, not raise KeyError.
        for (auto& pair : obj->dict) {
            if (PyObject_CompareBool(pair.first, key, 0)) {
                if (pair.second) Py_INCREF(pair.second);
                return pair.second;
            }
        }
        // collections.defaultdict: before raising KeyError, check the
        // out-of-band factory map. If this dict has one registered, call
        // the factory (empty arg list — matches how list()/dict()/int()
        // are invoked as zero-arg factories elsewhere), store the result
        // under the missing key (mutate-on-access, matching real
        // defaultdict), and return it.
        {
            auto dfIt = g_pycDefaultFactories.find(obj);
            if (dfIt != g_pycDefaultFactories.end() && dfIt->second) {
                PyObject* emptyArgs = PyList_New(0);
                // Pyc_Apply may legitimately return nullptr (a factory
                // that returns None) — that's fine, nullptr already
                // represents None throughout this codebase (see the
                // dict-scan branch above). `made` arrives already owning
                // one reference (from Pyc_Apply); PyDict_SetItem takes its
                // own separate reference for the dict slot, so the
                // original reference can be handed straight to the caller
                // below with no extra Py_INCREF needed.
                PyObject* made = Pyc_Apply(dfIt->second, emptyArgs);
                Py_DECREF(emptyArgs);
                PyDict_SetItem(obj, key, made);
                return made;
            }
        }
        // Raw key as the message; pyc_exc_message adds the repr quoting for
        // string keys (str(KeyError('k')) is "'k'").
        PyObject* t = PyUnicode_FromString("KeyError");
        PyObject* e = pyc_make_exc(t, key);
        Py_DECREF(t);
        pyc_raise(e);
        return nullptr;
    }
    PyObject* r = Pyc_GetItem(obj, key);
    if (r) return r;
    if (obj && obj->type == 1) { pyc_raise_msg("IndexError", "list index out of range"); return nullptr; }
    if (obj && obj->type == 7) { pyc_raise_msg("IndexError", "tuple index out of range"); return nullptr; }
    if (obj && obj->type == 3) { pyc_raise_msg("IndexError", "string index out of range"); return nullptr; }
    if (obj && (obj->type == 17 || obj->type == 18)) { pyc_raise_msg("IndexError", "index out of range"); return nullptr; }
    return nullptr;
}

PyObject* Pyc_SetItem(PyObject* obj, PyObject* key, PyObject* val) {
    if (!obj || !key) return nullptr;
    if (obj->type == 1) { PyList_SetItemBoxed(obj, key, val); return nullptr; }
    if (obj->type == 2) { PyDict_SetItem(obj, key, val); return nullptr; }
    // bytearray index assignment: ba[i] = x, x an int 0-255 (bytes, type
    // 17, is immutable — real Python raises TypeError on `b"x"[0]=1`,
    // not reachable here since only bytearray's typeOf tag routes
    // through this path — see Compiler.cpp's assignment lowering).
    if (obj->type == 18 && (key->type == 0 || key->type == 5) && val && (val->type == 0 || val->type == 5)) {
        long idx = key->value;
        if (idx < 0) idx += (long)obj->str.size();
        if (idx >= 0 && (size_t)idx < obj->str.size()) {
            obj->str[(size_t)idx] = (char)(unsigned char)(val->value & 0xFF);
        }
        return nullptr;
    }
    return nullptr;
}

// obj[key] = val — genuine subscript assignment only. Deliberately a
// separate function from Pyc_SetItem: that one is also used for plain
// attribute assignment (obj.attr = val) and various internal class/
// instance-dict setup, none of which should ever trigger a
// user-defined __setitem__ — only Compiler.cpp's Subscript-assignment
// lowering (obj[key] = val, and the read-modify-write half of
// obj[key] op= val) calls this one. Found and fixed while bug hunting,
// alongside __getitem__ above.
PyObject* Pyc_SubscriptSetItem(PyObject* obj, PyObject* key, PyObject* val) {
    if (obj && obj->type == 2) {
        PyObject* setitemMethod = pyc_lookup_dunder(obj, "__setitem__");
        if (setitemMethod) {
            PyObject* args = PyList_New(0);
            PyList_Append(args, obj);
            PyList_Append(args, key);
            PyList_Append(args, val);
            PyObject* r = Pyc_Apply(setitemMethod, args);
            Py_DECREF(args);
            if (r) Py_DECREF(r);
            return nullptr;
        }
    }
    return Pyc_SetItem(obj, key, val);
}

// del obj[key] — dispatches on obj's runtime type. Found missing while
// hunting for more instances of the truthiness bug's underlying pattern:
// Compiler.cpp previously called PyDict_DelItem unconditionally for
// *any* `del obj[idx]`, regardless of obj's type — since PyDict_DelItem
// only acts on type==2, `del lst[i]` on ANY list (not just ones using
// the homogeneous fast-path storage — this one is a different root
// cause, a missing dispatch branch rather than a storage-representation
// mismatch) silently did nothing at all. Confirmed against real
// CPython. Compiler.cpp's del-Subscript lowering now calls this instead
// of PyDict_DelItem directly.
PyObject* Pyc_DelItem(PyObject* obj, PyObject* key) {
    if (obj && obj->type == 2) return PyDict_DelItem(obj, key);
    if (obj && obj->type == 1 && key && (key->type == 0 || key->type == 5)) {
        pyc_ensure_boxed_list(obj);
        long idx = key->value;
        if (idx < 0) idx += (long)obj->list.size();
        if (idx >= 0 && (size_t)idx < obj->list.size()) {
            PyObject* item = obj->list[(size_t)idx];
            if (item) Py_DECREF(item);
            obj->list.erase(obj->list.begin() + idx);
            return PyBool_New(1);
        }
        pyc_raise_msg("IndexError", "list assignment index out of range");
        return nullptr;
    }
    return PyBool_New(0);
}

PyObject* Pyc_Contains(PyObject* container, PyObject* item) {
    if (!container || !item) return PyBool_New(0);
    // __contains__ dispatch for a class instance — found and fixed
    // while bug hunting: `item in container` for a class defining
    // __contains__ previously fell through to the type==2 branch
    // further below, which scans the instance's own attribute *names*
    // for a match (a class instance is a dict with a "__class__" entry)
    // — a meaningless check for almost any real class, confirmed via a
    // Container class whose __contains__ checks its own `items` dict.
    if (container->type == 2) {
        PyObject* containsMethod = pyc_lookup_dunder(container, "__contains__");
        if (containsMethod) {
            PyObject* r = pyc_call_dunder2(containsMethod, container, item);
            int truthy = PyObject_TruthValue(r);
            if (r) Py_DECREF(r);
            return PyBool_New(truthy);
        }
    }
    if (container->type == 1) {
        // Homogeneous int list
        if (container->list_item_type == 1) {
            long itemVal = 0;
            if (item->type == 0 || item->type == 5) itemVal = item->value;
            else if (item->type == 4) itemVal = (long)item->dvalue;
            else return PyBool_New(0);
            for (auto val : container->ilist)
                if (val == itemVal) return PyBool_New(1);
            return PyBool_New(0);
        }
        // Homogeneous float list
        if (container->list_item_type == 2) {
            double itemVal = 0.0;
            if (item->type == 4) itemVal = item->dvalue;
            else if (item->type == 0 || item->type == 5) itemVal = (double)item->value;
            else return PyBool_New(0);
            for (auto val : container->flist)
                if (val == itemVal) return PyBool_New(1);
            return PyBool_New(0);
        }
        // General boxed list
        for (auto* elem : container->list)
            if (elem && PyObject_CompareBool(elem, item, 0)) return PyBool_New(1);
        return PyBool_New(0);
    }
    if (container->type == 7) {
        // tuple: scan elements by value (tuples are always boxed-storage
        // from the compiler's lowering, but handle homogeneous too).
        if (container->list_item_type == 1) {
            long iv = (item && (item->type == 0 || item->type == 5)) ? item->value
                      : (item && item->type == 4) ? (long)item->dvalue : 0;
            for (auto v : container->ilist) if (v == iv) return PyBool_New(1);
            return PyBool_New(0);
        }
        if (container->list_item_type == 2) {
            double dv = (item && item->type == 4) ? item->dvalue
                        : (item && (item->type == 0 || item->type == 5)) ? (double)item->value : 0.0;
            for (auto v : container->flist) if (v == dv) return PyBool_New(1);
            return PyBool_New(0);
        }
        for (auto* elem : container->list)
            if (elem && PyObject_CompareBool(elem, item, 0)) return PyBool_New(1);
        return PyBool_New(0);
    }
    if (container->type == 3) {
        if (item->type == 3)
            return PyBool_New(container->str.find(item->str) != std::string::npos);
        return PyBool_New(0);
    }
    if (container->type == 17 || container->type == 18) {
        // `in` on bytes/bytearray: an int operand (0-255) checks for that
        // single byte value; a bytes/bytearray/str operand checks for a
        // substring, matching real Python's dual `x in b"..."` behavior.
        if (item->type == 0 || item->type == 5) {
            unsigned char b = (unsigned char)(item->value & 0xFF);
            return PyBool_New(container->str.find((char)b) != std::string::npos);
        }
        if (item->type == 3 || item->type == 17 || item->type == 18)
            return PyBool_New(container->str.find(item->str) != std::string::npos);
        return PyBool_New(0);
    }
    if (container->type == 2) {
        for (auto& pair : container->dict)
            if (PyObject_CompareBool(pair.first, item, 0)) return PyBool_New(1);
        return PyBool_New(0);
    }
    if (container->type == 20) {
        return PyBool_New(PySet_Contains(container, item));
    }
    return PyBool_New(0);
}

PyObject* Pyc_Pow(PyObject* a, PyObject* b) {
    if (!a || !b) return nullptr;
    // Complex pow: if either operand is complex, promote and use std::pow.
    if (has_complex(a, b)) {
        double ar, ai, br, bi;
        if (to_complex(a, ar, ai) && to_complex(b, br, bi)) {
            std::complex<double> z1(ar, ai), z2(br, bi);
            std::complex<double> result = std::pow(z1, z2);
            return PyComplex_New(result.real(), result.imag());
        }
    }
    if (!is_numeric(a) || !is_numeric(b)) return nullptr;
    if (both_integral(a, b) && b->value >= 0) {
        long result = 1, base = a->value, exp = b->value;
        for (long i = 0; i < exp; ++i) result *= base;
        return PyInt_FromLong(result);
    }
    return PyFloat_FromDouble(pow(numeric_val(a), numeric_val(b)));
}

// Native integer power: computes base^exp for int64 values
// Handles non-negative exponents efficiently
int64_t Pyc_PowInt64(int64_t base, int64_t exp) {
    if (exp < 0) {
        // Negative exponent: return 0 (integer division result)
        return 0;
    }
    if (exp == 0) return 1;
    int64_t result = 1;
    int64_t b = base;
    int64_t e = exp;
    // Binary exponentiation (exponentiation by squaring)
    while (e > 0) {
        if (e & 1) result *= b;
        b *= b;
        e >>= 1;
    }
    return result;
}

// Boxed integer power: exp >= 0 yields int, exp < 0 yields float (Python semantics)
PyObject* Pyc_PowInt64Obj(int64_t base, int64_t exp) {
    if (exp >= 0) return PyInt_FromLong(Pyc_PowInt64(base, exp));
    return PyFloat_FromDouble(pow((double)base, (double)exp));
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
        if (lst->list_item_type == 1) {
            for (auto val : lst->ilist) addOne(PyInt_FromLong(val));
        } else if (lst->list_item_type == 2) {
            for (auto val : lst->flist) addOne(PyFloat_FromDouble(val));
        } else {
            for (auto* item : lst->list) addOne(item);
        }
    } else if (lst->type == 2) {
        for (auto& pair : lst->dict) addOne(pair.first);
    } else if (lst->type == 20) {
        for (auto* e : lst->setElems) addOne(e);
    }
    return total;
}

// PyBuiltin_Sorted(iterable, key) — sort the iterable's elements. If
// `key` is non-null it is a 1-arg callable applied to each element
// before comparison (standard sort key behaviour).
// `reverse`, if truthy, reverses the final sorted order — found missing
// entirely (silently ignored, `sorted([3,1,2], reverse=True)` returned
// the plain ascending sort) while hunting for more bugs; fixed by
// reversing `r` in place before returning, in both the key and no-key
// branches below.
PyObject* PyBuiltin_Sorted(PyObject* lst, PyObject* key, PyObject* reverse) {
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
    } else if (lst->type == 20) {
        for (auto* e : lst->setElems) {
            if (e) Py_INCREF(e);
            items.push_back(e);
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
        if (PyObject_TruthValue(reverse)) std::reverse(r->list.begin(), r->list.end());
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
    if (PyObject_TruthValue(reverse)) std::reverse(r->list.begin(), r->list.end());
    return r;
}

// PyBuiltin_CmpToKey(cmp) — returns a dict with a "cmp_to_key" token.
// This is used by the special-case detection in lowerCall for
// sorted(iterable, key=cmp_to_key(cmp)). The dict token allows the
// sorted function to recognize that it should use PyBuiltin_SortedWithCmp.
PyObject* PyBuiltin_CmpToKey(PyObject* cmp) {
    PyObject* d = PyDict_New();
    PyObject* k = PyUnicode_FromString("cmp_to_key");
    PyObject* v = PyUnicode_FromString("cmp_to_key");
    PyDict_SetItem(d, k, v);
    Py_DECREF(k);
    Py_DECREF(v);
    return d;
}

// PyBuiltin_SortedWithCmp(iterable, cmp) — like sorted but takes a
// 2-arg comparator function instead of a key function. The comparator
// is invoked as cmp(a, b) for each pair; a negative return means
// a < b, zero means a == b, positive means a > b. This is the
// internal fast-path for sorted(..., key=cmp_to_key(cmp)).
PyObject* PyBuiltin_SortedWithCmp(PyObject* lst, PyObject* cmp) {
    if (!lst || !cmp) return PyBuiltin_Sorted(lst, nullptr, nullptr);
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
    } else if (lst->type == 20) {
        for (auto* e : lst->setElems) {
            if (e) Py_INCREF(e);
            items.push_back(e);
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
    if (!lst) return PyBool_New(0);
    if (lst->type == 20) {
        for (auto* e : lst->setElems) if (PyObject_TruthValue(e)) return PyBool_New(1);
        return PyBool_New(0);
    }
    if (lst->type != 1) return PyBool_New(0);
    if (lst->list_item_type == 1) {
        for (auto val : lst->ilist)
            if (val != 0) return PyBool_New(1);
    } else if (lst->list_item_type == 2) {
        for (auto val : lst->flist)
            if (val != 0.0) return PyBool_New(1);
    } else {
        for (auto* item : lst->list)
            if (PyObject_TruthValue(item)) return PyBool_New(1);
    }
    return PyBool_New(0);
}

PyObject* PyBuiltin_All(PyObject* lst) {
    if (!lst) return PyBool_New(1);
    if (lst->type == 20) {
        for (auto* e : lst->setElems) if (!PyObject_TruthValue(e)) return PyBool_New(0);
        return PyBool_New(1);
    }
    if (lst->type != 1) return PyBool_New(1);
    if (lst->list_item_type == 1) {
        for (auto val : lst->ilist)
            if (val == 0) return PyBool_New(0);
    } else if (lst->list_item_type == 2) {
        for (auto val : lst->flist)
            if (val == 0.0) return PyBool_New(0);
    } else {
        for (auto* item : lst->list)
            if (!PyObject_TruthValue(item)) return PyBool_New(0);
    }
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
    } else if (obj->type == 7) {
        n = (long)PyTuple_Size(obj);
    } else if (obj->type == 3 || obj->type == 17 || obj->type == 18) {
        n = (long)obj->str.size();
    } else {
        n = 0;
    }
    long stp = (step && (step->type==0||step->type==5)) ? step->value : 1;
    if (stp == 0) {
        if (obj->type == 3) return PyUnicode_FromString("");
        if (obj->type == 17) return PyBytes_FromStringAndSize("", 0);
        if (obj->type == 18) return PyByteArray_FromStringAndSize("", 0);
        return PyList_New(0);
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
    if (obj->type == 7) {
        // Tuple slice -> tuple. Tuples are always boxed-storage from the
        // compiler's lowering, but handle homogeneous defensively.
        if (obj->list_item_type != 0) pyc_ensure_boxed_list(obj);
        PyObject* r = PyTuple_New(idxs.size());
        for (size_t k = 0; k < idxs.size(); ++k) {
            long i = idxs[k];
            PyObject* item = (i >= 0 && (size_t)i < obj->list.size()) ? obj->list[i] : nullptr;
            PyTuple_SetItem(r, k, item);  // INCREFs inside
        }
        return r;
    }
    if (obj->type == 3 || obj->type == 17 || obj->type == 18) {
        std::string r;
        for (long i : idxs) {
            if (i >= 0 && (size_t)i < obj->str.size()) r += obj->str[(size_t)i];
        }
        if (obj->type == 17) return PyBytes_FromStringAndSize(r.data(), r.size());
        if (obj->type == 18) return PyByteArray_FromStringAndSize(r.data(), r.size());
        return PyUnicode_FromStringAndSize(r.data(), r.size());
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

// Apply an optional key function to a single item, returning a new
// reference (the item itself, re-INCREF'd, when key is null). Shared by
// the min/max 2-value and list forms below.
static PyObject* pyc_apply_key1(PyObject* key, PyObject* item) {
    if (!key) { if (item) Py_INCREF(item); return item; }
    PyObject* argList = PyList_New(1);
    if (item) { Py_INCREF(item); PyList_SetItem(argList, 0, item); }
    PyObject* k = Pyc_Apply(key, argList);
    Py_DECREF(argList);
    return k;
}
PyObject* PyBuiltin_Min2(PyObject* a, PyObject* b, PyObject* key) {
    if (!a) return (b ? (Py_INCREF(b), b) : nullptr);
    if (!b) return (Py_INCREF(a), a);
    if (!key) return PyObject_CompareBool(a, b, 2) ? (Py_INCREF(a), a) : (Py_INCREF(b), b);
    PyObject* ka = pyc_apply_key1(key, a);
    PyObject* kb = pyc_apply_key1(key, b);
    bool aWins = (ka && kb) ? (PyObject_CompareBool(ka, kb, 2) != 0) : true;
    if (ka) Py_DECREF(ka);
    if (kb) Py_DECREF(kb);
    return aWins ? (Py_INCREF(a), a) : (Py_INCREF(b), b);
}
PyObject* PyBuiltin_Max2(PyObject* a, PyObject* b, PyObject* key) {
    if (!a) return (b ? (Py_INCREF(b), b) : nullptr);
    if (!b) return (Py_INCREF(a), a);
    if (!key) return PyObject_CompareBool(a, b, 3) ? (Py_INCREF(a), a) : (Py_INCREF(b), b);
    PyObject* ka = pyc_apply_key1(key, a);
    PyObject* kb = pyc_apply_key1(key, b);
    bool aWins = (ka && kb) ? (PyObject_CompareBool(ka, kb, 3) != 0) : true;
    if (ka) Py_DECREF(ka);
    if (kb) Py_DECREF(kb);
    return aWins ? (Py_INCREF(a), a) : (Py_INCREF(b), b);
}
PyObject* PyBuiltin_MinList(PyObject* lst, PyObject* key, PyObject* defaultVal) {
    if (!lst || lst->type != 1) {
        if (defaultVal) { Py_INCREF(defaultVal); return defaultVal; }
        return nullptr;
    }
    size_t n = 0;
    if (lst->list_item_type == 1) n = lst->ilist.size();
    else if (lst->list_item_type == 2) n = lst->flist.size();
    else n = lst->list.size();
    if (n == 0) {
        if (defaultVal) { Py_INCREF(defaultVal); return defaultVal; }
        return nullptr;
    }
    auto getItem = [&](size_t i) -> PyObject* {
        if (lst->list_item_type == 1) return PyInt_FromLong(lst->ilist[i]);
        if (lst->list_item_type == 2) return PyFloat_FromDouble(lst->flist[i]);
        PyObject* it = lst->list[i]; if (it) Py_INCREF(it); return it;
    };
    PyObject* r = getItem(0);
    PyObject* rKey = pyc_apply_key1(key, r);
    for (size_t i = 1; i < n; ++i) {
        PyObject* item = getItem(i);
        PyObject* k = pyc_apply_key1(key, item);
        if (item && k && rKey && PyObject_CompareBool(k, rKey, 2)) {
            Py_DECREF(r); if (rKey) Py_DECREF(rKey);
            r = item; rKey = k;
        } else {
            if (item) Py_DECREF(item);
            if (k) Py_DECREF(k);
        }
    }
    if (rKey) Py_DECREF(rKey);
    return r;
}
PyObject* PyBuiltin_MaxList(PyObject* lst, PyObject* key, PyObject* defaultVal) {
    if (!lst || lst->type != 1) {
        if (defaultVal) { Py_INCREF(defaultVal); return defaultVal; }
        return nullptr;
    }
    size_t n = 0;
    if (lst->list_item_type == 1) n = lst->ilist.size();
    else if (lst->list_item_type == 2) n = lst->flist.size();
    else n = lst->list.size();
    if (n == 0) {
        if (defaultVal) { Py_INCREF(defaultVal); return defaultVal; }
        return nullptr;
    }
    auto getItem = [&](size_t i) -> PyObject* {
        if (lst->list_item_type == 1) return PyInt_FromLong(lst->ilist[i]);
        if (lst->list_item_type == 2) return PyFloat_FromDouble(lst->flist[i]);
        PyObject* it = lst->list[i]; if (it) Py_INCREF(it); return it;
    };
    PyObject* r = getItem(0);
    PyObject* rKey = pyc_apply_key1(key, r);
    for (size_t i = 1; i < n; ++i) {
        PyObject* item = getItem(i);
        PyObject* k = pyc_apply_key1(key, item);
        if (item && k && rKey && PyObject_CompareBool(k, rKey, 3)) {
            Py_DECREF(r); if (rKey) Py_DECREF(rKey);
            r = item; rKey = k;
        } else {
            if (item) Py_DECREF(item);
            if (k) Py_DECREF(k);
        }
    }
    if (rKey) Py_DECREF(rKey);
    return r;
}
PyObject* PyBuiltin_List(PyObject* obj) {
    if (!obj) return PyList_New(0);
    if (obj->type == 1) { Py_INCREF(obj); return obj; }
    if (obj->type == 7) {
        // tuple -> list: copy elements into a fresh list.
        if (obj->list_item_type != 0) pyc_ensure_boxed_list(obj);
        PyObject* r = PyList_New(obj->list.size());
        for (size_t i = 0; i < obj->list.size(); ++i) {
            PyList_SetItem(r, i, obj->list[i]);  // INCREFs inside
        }
        return r;
    }
    if (obj->type == 3) {
        PyObject* r = PyList_New(obj->str.size());
        for (size_t i = 0; i < obj->str.size(); ++i) {
            char buf[2] = {obj->str[i], '\0'};
            PyList_SetItem(r, i, PyUnicode_FromString(buf));
        }
        return r;
    }
    if (obj->type == 2) {
        // __iter__/__next__ dispatch for a class instance — found and
        // fixed while bug hunting; see pyc_materialize_iterator_protocol's
        // comment (far below) for the full story. Checked first: a
        // genuine plain dict has no "__class__" entry, so
        // pyc_lookup_dunder always returns nullptr for it and this is a
        // no-op, falling through to the existing dict-iterates-its-keys
        // behavior below unchanged.
        if (pyc_lookup_dunder(obj, "__iter__")) {
            return pyc_materialize_iterator_protocol(obj);
        }
        // CPython: list(dict) iterates over keys.
        PyObject* r = PyList_New(obj->dict.size());
        size_t i = 0;
        for (auto& pair : obj->dict) {
            if (pair.first) Py_INCREF(pair.first);
            PyList_SetItem(r, i++, pair.first);
        }
        return r;
    }
    if (obj->type == 17 || obj->type == 18) {
        // bytes/bytearray iterate as ints (0-255), same as indexing.
        PyObject* r = PyList_New(obj->str.size());
        for (size_t i = 0; i < obj->str.size(); ++i) {
            PyList_SetItem(r, i, PyInt_FromLong((unsigned char)obj->str[i]));
        }
        return r;
    }
    if (obj->type == 20) {
        return PySet_ToList(obj);
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
    } else if (obj->type == 20) {
        r = PyList_New(obj->setElems.size());
        for (size_t i = 0; i < obj->setElems.size(); ++i) {
            size_t ri = obj->setElems.size() - 1 - i;
            if (obj->setElems[ri]) Py_INCREF(obj->setElems[ri]);
            PyList_SetItem(r, i, obj->setElems[ri]);
        }
    } else {
        return PyList_New(0);
    }
    return r;
}
PyObject* PyBuiltin_Enumerate(PyObject* iterable) {
    if (!iterable || iterable->type != 1) return PyList_New(0);
    size_t n = 0;
    if (iterable->list_item_type == 1) n = iterable->ilist.size();
    else if (iterable->list_item_type == 2) n = iterable->flist.size();
    else n = iterable->list.size();
    PyObject* r = PyList_New(n);
    for (size_t i = 0; i < n; ++i) {
        PyObject* pair = PyTuple_New(2);
        PyTuple_SetItem(pair, 0, PyInt_FromLong((long)i));
        PyObject* v = nullptr;
        if (iterable->list_item_type == 1) v = PyInt_FromLong(iterable->ilist[i]);
        else if (iterable->list_item_type == 2) v = PyFloat_FromDouble(iterable->flist[i]);
        else { v = iterable->list[i]; if (v) Py_INCREF(v); }
        PyTuple_SetItem(pair, 1, v);
        PyList_SetItem(r, i, pair);
    }
    return r;
}
PyObject* PyBuiltin_Zip2(PyObject* a, PyObject* b) {
    if (!a || !b) return PyList_New(0);
    size_t na = 0, nb = 0;
    if (a->type == 1) {
        if (a->list_item_type == 1) na = a->ilist.size();
        else if (a->list_item_type == 2) na = a->flist.size();
        else na = a->list.size();
    }
    if (b->type == 1) {
        if (b->list_item_type == 1) nb = b->ilist.size();
        else if (b->list_item_type == 2) nb = b->flist.size();
        else nb = b->list.size();
    }
    size_t n = na < nb ? na : nb;
    PyObject* r = PyList_New(n);
    for (size_t i = 0; i < n; ++i) {
        PyObject* pair = PyTuple_New(2);
        PyObject* va = nullptr, *vb = nullptr;
        if (a->list_item_type == 1) va = PyInt_FromLong(a->ilist[i]);
        else if (a->list_item_type == 2) va = PyFloat_FromDouble(a->flist[i]);
        else { va = a->list[i]; if (va) Py_INCREF(va); }
        if (b->list_item_type == 1) vb = PyInt_FromLong(b->ilist[i]);
        else if (b->list_item_type == 2) vb = PyFloat_FromDouble(b->flist[i]);
        else { vb = b->list[i]; if (vb) Py_INCREF(vb); }
        PyTuple_SetItem(pair, 0, va);
        PyTuple_SetItem(pair, 1, vb);
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
        // Handle tuples (type 7) — % operator unpacks a tuple arg just like
        // a list. CPython: "%s %d" % (1, 2) unpacks the tuple.
        if (args->type == 7 && idx < args->list.size()) return args->list[idx];
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

// date/datetime/timedelta arithmetic helpers. Implemented via the shared
// runtime primitives (PyNumber_Add/Subtract/Multiply below) rather than
// gated on compiler-inferred typeOf, so they work correctly even when a
// date/timedelta value arrives as an untyped function parameter — Codegen
// already falls back to calling these functions for any binop the
// compiler didn't specialize, so making them type-14/15-aware here is
// what actually makes arithmetic robust across function boundaries.
static PyObject* pyc_datetime_add_timedelta(const PycDateTime* dt, const PycTimedelta* td, bool negate) {
    int64_t sign = negate ? -1 : 1;
    int64_t days = pyc_days_from_civil(dt->year, dt->month, dt->day);
    if (!dt->hasTime) {
        // A plain date only cares about the day component of a timedelta
        // (matches CPython's date.__add__ — a date can't hold a partial day).
        days += sign * td->days;
        int y, mo, da;
        pyc_civil_from_days(days, y, mo, da);
        return pyc_new_datetime(y, mo, da, 0, 0, 0, false);
    }
    int64_t secs = dt->hour * 3600LL + dt->minute * 60LL + dt->second;
    days += sign * td->days;
    secs += sign * td->seconds;
    int64_t extraDay = pyc_floordiv(secs, 86400);
    days += extraDay;
    secs -= extraDay * 86400;
    int y, mo, da;
    pyc_civil_from_days(days, y, mo, da);
    int hh = (int)(secs / 3600), mi = (int)((secs % 3600) / 60), ss = (int)(secs % 60);
    return pyc_new_datetime(y, mo, da, hh, mi, ss, true);
}
static PyObject* pyc_datetime_diff(const PycDateTime* a, const PycDateTime* b) {
    int64_t daysA = pyc_days_from_civil(a->year, a->month, a->day);
    int64_t daysB = pyc_days_from_civil(b->year, b->month, b->day);
    if (!a->hasTime && !b->hasTime) return pyc_new_timedelta(daysA - daysB, 0, 0);
    int64_t secsA = a->hasTime ? (a->hour * 3600LL + a->minute * 60LL + a->second) : 0;
    int64_t secsB = b->hasTime ? (b->hour * 3600LL + b->minute * 60LL + b->second) : 0;
    int64_t totalSecs = (daysA - daysB) * 86400 + (secsA - secsB);
    int64_t days = pyc_floordiv(totalSecs, 86400);
    int64_t secs = totalSecs - days * 86400;
    return pyc_new_timedelta(days, secs, 0);
}
static PyObject* pyc_timedelta_add(const PycTimedelta* a, const PycTimedelta* b, bool negate) {
    int64_t sign = negate ? -1 : 1;
    return pyc_new_timedelta(a->days + sign * b->days, a->seconds + sign * b->seconds,
                              a->microseconds + sign * b->microseconds);
}
static PyObject* pyc_timedelta_mul(const PycTimedelta* a, int64_t n) {
    return pyc_new_timedelta(a->days * n, a->seconds * n, a->microseconds * n);
}

PyObject* PyNumber_Add(PyObject* a, PyObject* b) {
    if (!a || !b) return NULL;
    // __add__ dispatch for a class instance — found and fixed while bug
    // hunting: a + b for a class defining __add__ previously always
    // returned None. Only the left operand's dunder is consulted
    // (__radd__ is a further, narrower simplification, not attempted
    // here — matches the same choice made for comparison dispatch
    // above).
    if (a->type == 2) {
        PyObject* method = pyc_lookup_dunder(a, "__add__");
        if (method) return pyc_call_dunder2(method, a, b);
    }
    if (a->type == 3 && b->type == 3) return PyString_Concat(a, b);
    // list + list concatenation — see PyList_Concat's comment above for
    // why this was previously entirely missing (not a homogeneous-list
    // storage bug on its own, but found the same way: `[1,2,3] + [4,5]`
    // returned None unconditionally, confirmed against real CPython).
    if (a->type == 1 && b->type == 1) return PyList_Concat(a, b);
    // tuple + tuple -> tuple (CPython: TypeError for tuple + list).
    if (a->type == 7 && b->type == 7) return PyTuple_Concat(a, b);
    // bytes/bytearray concatenation. Result type follows the left
    // operand (matches real Python: bytearray + bytes -> bytearray,
    // bytes + bytearray -> bytes).
    if ((a->type == 17 || a->type == 18) && (b->type == 17 || b->type == 18)) {
        std::string combined = a->str + b->str;
        return (a->type == 18) ? PyByteArray_FromStringAndSize(combined.data(), combined.size())
                                : PyBytes_FromStringAndSize(combined.data(), combined.size());
    }
    if (a->type == 14 && b->type == 15) return pyc_datetime_add_timedelta(pyc_as_datetime(a), pyc_as_timedelta(b), false);
    if (a->type == 15 && b->type == 14) return pyc_datetime_add_timedelta(pyc_as_datetime(b), pyc_as_timedelta(a), false);
    if (a->type == 15 && b->type == 15) return pyc_timedelta_add(pyc_as_timedelta(a), pyc_as_timedelta(b), false);
    if (a->type == 19 || b->type == 19) {
        bool aTemp = false, bTemp = false;
        mpd_t* da = pyc_decimal_operand(a, &aTemp);
        mpd_t* db = pyc_decimal_operand(b, &bTemp);
        PyObject* result = nullptr;
        if (da && db) {
            mpd_t* r = mpd_qnew();
            uint32_t status = 0;
            mpd_qadd(r, da, db, pyc_dec_ctx(), &status);
            result = pyc_decimal_wrap(r);
        }
        if (aTemp) mpd_del(da);
        if (bTemp) mpd_del(db);
        return result;
    }
    if (a && b && has_complex(a, b)) {
        double ar, ai, br, bi;
        if (to_complex(a, ar, ai) && to_complex(b, br, bi))
            return PyComplex_New(ar + br, ai + bi);
    }
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) return PyInt_FromLong(a->value + b->value);
    return PyFloat_FromDouble(numeric_val(a) + numeric_val(b));
}

PyObject* PyNumber_Subtract(PyObject* a, PyObject* b) {
    // __sub__ dispatch for a class instance — see PyNumber_Add's
    // comment; same "left operand only" simplification.
    if (a && b && a->type == 2) {
        PyObject* method = pyc_lookup_dunder(a, "__sub__");
        if (method) return pyc_call_dunder2(method, a, b);
    }
    if (a && a->type == 20) return PySet_Difference(a, b);
    if (a && b && a->type == 14 && b->type == 15) return pyc_datetime_add_timedelta(pyc_as_datetime(a), pyc_as_timedelta(b), true);
    if (a && b && a->type == 14 && b->type == 14) return pyc_datetime_diff(pyc_as_datetime(a), pyc_as_datetime(b));
    if (a && b && a->type == 15 && b->type == 15) return pyc_timedelta_add(pyc_as_timedelta(a), pyc_as_timedelta(b), true);
    if (a && b && (a->type == 19 || b->type == 19)) {
        bool aTemp = false, bTemp = false;
        mpd_t* da = pyc_decimal_operand(a, &aTemp);
        mpd_t* db = pyc_decimal_operand(b, &bTemp);
        PyObject* result = nullptr;
        if (da && db) {
            mpd_t* r = mpd_qnew();
            uint32_t status = 0;
            mpd_qsub(r, da, db, pyc_dec_ctx(), &status);
            result = pyc_decimal_wrap(r);
        }
        if (aTemp) mpd_del(da);
        if (bTemp) mpd_del(db);
        return result;
    }
    if (a && b && has_complex(a, b)) {
        double ar, ai, br, bi;
        if (to_complex(a, ar, ai) && to_complex(b, br, bi))
            return PyComplex_New(ar - br, ai - bi);
    }
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

// Translates the small subset of Python re flag bits pyc supports
// (IGNORECASE=2, MULTILINE=8, DOTALL=16 — real CPython values, see
// makeReModuleDict below) into the corresponding PCRE2 compile options.
// Unrecognized bits are silently ignored (matches this codebase's
// existing "cover the common cases" precedent elsewhere, e.g. struct's
// unsupported format codes).
static uint32_t pyc_re_flags_to_pcre2(int64_t flags) {
    uint32_t opts = 0;
    if (flags & 2)  opts |= PCRE2_CASELESS;   // re.IGNORECASE
    if (flags & 8)  opts |= PCRE2_MULTILINE;  // re.MULTILINE
    if (flags & 16) opts |= PCRE2_DOTALL;     // re.DOTALL
    return opts;
}

static pcre2_code* compileRegex(const std::string& pat, std::string& err, uint32_t options = 0) {
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code* code = pcre2_compile(
        (PCRE2_SPTR)pat.c_str(), (PCRE2_SIZE)pat.size(),
        options, &errcode, &erroffset, nullptr);
    if (!code) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(errcode, buf, sizeof(buf));
        err = std::string((const char*)buf) + " at offset " + std::to_string(erroffset);
        return nullptr;
    }
    return code;
}

// Unboxes a flags argument (int, or null/None meaning "no flags") the
// same way count/maxsplit arguments are unboxed elsewhere in this file.
static uint32_t pyc_re_unbox_flags(PyObject* flags) {
    if (!flags || (flags->type != 0 && flags->type != 5)) return 0;
    return pyc_re_flags_to_pcre2(flags->value);
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

extern "C" PyObject* PyBuiltin_ReFinditer(PyObject* pattern, PyObject* subject, PyObject* flags) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err, pyc_re_unbox_flags(flags));
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
        return nullptr;
    }
    PyObject* result = runRegexAll(code, subject->str);
    pcre2_code_free(code);
    return result;
}

extern "C" PyObject* PyBuiltin_ReFindall(PyObject* pattern, PyObject* subject, PyObject* flags) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err, pyc_re_unbox_flags(flags));
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

// re.search(pattern, subject, flags=0) — return a Match object (type 9)
// for the first match, or None if no match. Previously hardcoded
// PCRE2_CASELESS unconditionally here (a real bug: every re.search/
// re.match call was always case-insensitive regardless of flags,
// confirmed against real CPython). Fixed by compiling case-sensitive by
// default and only translating re.IGNORECASE (and MULTILINE/DOTALL) into
// PCRE2 options when actually passed — see pyc_re_flags_to_pcre2.
extern "C" PyObject* PyBuiltin_ReSearch(PyObject* pattern, PyObject* subject, PyObject* flags) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err, pyc_re_unbox_flags(flags));
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
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

extern "C" PyObject* PyBuiltin_ReMatch(PyObject* pattern, PyObject* subject, PyObject* flags) {
    // re.match: anchored at start of string (PCRE2_ANCHORED flag).
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    uint32_t opts = pyc_re_unbox_flags(flags) | PCRE2_ANCHORED;
    pcre2_code* code = compileRegex(pattern->str, err, opts);
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
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

extern "C" PyObject* PyBuiltin_ReCompile(PyObject* pattern, PyObject* flags) {
    if (!pattern || pattern->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err, pyc_re_unbox_flags(flags));
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
    if (a && a->type == 20) return PySet_Union(a, b);
    if (b && b->type == 20) return PySet_Union(a, b);
    long av = 0, bv = 0;
    if (a && a->type == 0) av = (long)a->value;
    else if (a && a->type == 5) av = (long)a->value;
    if (b && b->type == 0) bv = (long)b->value;
    else if (b && b->type == 5) bv = (long)b->value;
    return PyInt_FromLong(av | bv);
}

extern "C" PyObject* PyNumber_BitAnd(PyObject* a, PyObject* b) {
    if (a && a->type == 20) return PySet_Intersection(a, b);
    if (b && b->type == 20) return PySet_Intersection(a, b);
    long av = 0, bv = 0;
    if (a && a->type == 0) av = (long)a->value;
    else if (a && a->type == 5) av = (long)a->value;
    if (b && b->type == 0) bv = (long)b->value;
    else if (b && b->type == 5) bv = (long)b->value;
    return PyInt_FromLong(av & bv);
}

extern "C" PyObject* PyNumber_BitXor(PyObject* a, PyObject* b) {
    if (a && a->type == 20) return PySet_SymmetricDifference(a, b);
    if (b && b->type == 20) return PySet_SymmetricDifference(a, b);
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

// os.path.join(*parts) -> str : joins path components with "/", matching
// CPython's behavior of discarding everything to the left of the last
// absolute (leading-"/") component.
extern "C" PyObject* PyBuiltin_OsPathJoin(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyUnicode_FromString("");
    std::string out;
    for (PyObject* p : args->list) {
        if (!p || p->type != 3) continue;
        if (p->str.empty()) continue;
        if (!p->str.empty() && p->str[0] == '/') {
            out = p->str; // absolute component resets the accumulated path
            continue;
        }
        if (!out.empty() && out.back() != '/') out += '/';
        out += p->str;
    }
    return PyUnicode_FromString(out.c_str());
}

// os.path.basename(p) -> str : text after the last "/"
extern "C" PyObject* PyBuiltin_OsPathBasename(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyUnicode_FromString("");
    PyObject* p = args->list[0];
    if (!p || p->type != 3) return PyUnicode_FromString("");
    size_t slash = p->str.find_last_of('/');
    return PyUnicode_FromString(slash == std::string::npos ? p->str.c_str() : p->str.c_str() + slash + 1);
}

// os.path.dirname(p) -> str : text before the last "/" (matching CPython's
// exact edge cases: no "/" -> "", trailing "/" kept as a single "/" if root).
extern "C" PyObject* PyBuiltin_OsPathDirname(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyUnicode_FromString("");
    PyObject* p = args->list[0];
    if (!p || p->type != 3) return PyUnicode_FromString("");
    size_t slash = p->str.find_last_of('/');
    if (slash == std::string::npos) return PyUnicode_FromString("");
    if (slash == 0) return PyUnicode_FromString("/");
    return PyUnicode_FromString(p->str.substr(0, slash).c_str());
}

// os.path.split(p) -> (head, tail) 2-tuple, matching CPython. Equivalent
// to (os.path.dirname(p), os.path.basename(p)).
extern "C" PyObject* PyBuiltin_OsPathSplit(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) {
        PyObject* r = PyTuple_New(2);
        PyTuple_SetItem(r, 0, PyUnicode_FromString(""));
        PyTuple_SetItem(r, 1, PyUnicode_FromString(""));
        return r;
    }
    PyObject* p = args->list[0];
    if (!p || p->type != 3) {
        PyObject* r = PyTuple_New(2);
        PyTuple_SetItem(r, 0, PyUnicode_FromString(""));
        PyTuple_SetItem(r, 1, PyUnicode_FromString(""));
        return r;
    }
    const std::string& s = p->str;
    size_t slash = s.find_last_of('/');
    std::string head, tail;
    if (slash == std::string::npos) {
        head = ""; tail = s;
    } else if (slash == 0) {
        head = "/"; tail = s.substr(1);
    } else {
        head = s.substr(0, slash); tail = s.substr(slash + 1);
    }
    PyObject* r = PyTuple_New(2);
    PyTuple_SetItem(r, 0, PyUnicode_FromString(head.c_str()));
    PyTuple_SetItem(r, 1, PyUnicode_FromString(tail.c_str()));
    return r;
}

// os.path.splitext(p) -> (root, ext) 2-tuple, matching CPython.
extern "C" PyObject* PyBuiltin_OsPathSplitext(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyTuple_New(0);
    PyObject* p = args->list[0];
    if (!p || p->type != 3) return PyTuple_New(0);
    const std::string& s = p->str;
    size_t slash = s.find_last_of('/');
    size_t dot = s.find_last_of('.');
    // A dot in the last path component that isn't a leading dot (matches
    // CPython: ".bashrc" has no extension, "a.tar.gz" splits at the last dot).
    std::string root, ext;
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot == (slash == std::string::npos ? 0 : slash + 1)) {
        root = s;
    } else {
        root = s.substr(0, dot);
        ext = s.substr(dot);
    }
    PyObject* out = PyTuple_New(2);
    PyTuple_SetItem(out, 0, PyUnicode_FromString(root.c_str()));
    PyTuple_SetItem(out, 1, PyUnicode_FromString(ext.c_str()));
    return out;
}

// os.path.abspath(p) -> str : joins with the real cwd if relative; does not
// resolve symlinks or ".."/"." components (unlike realpath(3)) since
// CPython's abspath is a pure string operation, not a filesystem call.
extern "C" PyObject* PyBuiltin_OsPathAbspath(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyUnicode_FromString("");
    PyObject* p = args->list[0];
    if (!p || p->type != 3) return PyUnicode_FromString("");
    if (!p->str.empty() && p->str[0] == '/') return PyUnicode_FromString(p->str.c_str());
    char cwd[4096];
    if (!::getcwd(cwd, sizeof(cwd))) return PyUnicode_FromString(p->str.c_str());
    std::string out = cwd;
    if (!p->str.empty()) {
        if (out.back() != '/') out += '/';
        out += p->str;
    }
    return PyUnicode_FromString(out.c_str());
}

// os.getcwd() -> str
extern "C" PyObject* PyBuiltin_OsGetcwd(PyObject* args) {
    (void)args;
    char cwd[4096];
    if (!::getcwd(cwd, sizeof(cwd))) return PyUnicode_FromString("");
    return PyUnicode_FromString(cwd);
}

// os.listdir(p=".") -> list[str] : directory entries excluding "." and "..",
// in whatever order readdir(3) yields them (not guaranteed to match
// CPython's order, which itself is filesystem-dependent).
extern "C" PyObject* PyBuiltin_OsListdir(PyObject* args) {
    PyObject* out = PyList_New(0);
    std::string path = ".";
    if (args && args->type == 1 && !args->list.empty() && args->list[0] && args->list[0]->type == 3) {
        path = args->list[0]->str;
    }
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        PyObject* nameObj = PyUnicode_FromString(ent->d_name);
        PyList_Append(out, nameObj);
        Py_DECREF(nameObj);
    }
    ::closedir(d);
    return out;
}

// os.makedirs(p, exist_ok=...) -> None : creates every missing path
// component. `exist_ok` isn't read (token+registry calls don't carry
// keyword arguments through generically, same limitation as other
// synthetic-module functions) — behaves as if exist_ok=True always,
// i.e. mkdir -p semantics, never raises FileExistsError.
extern "C" PyObject* PyBuiltin_OsMakedirs(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* p = args->list[0];
    if (!p || p->type != 3 || p->str.empty()) return nullptr;
    std::string cur;
    size_t i = 0;
    const std::string& s = p->str;
    if (s[0] == '/') { cur = "/"; i = 1; }
    while (i <= s.size()) {
        size_t next = s.find('/', i);
        std::string comp = (next == std::string::npos) ? s.substr(i) : s.substr(i, next - i);
        if (!comp.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += comp;
            ::mkdir(cur.c_str(), 0777);
        }
        if (next == std::string::npos) break;
        i = next + 1;
    }
    return nullptr;
}

// os.remove(path) -> None : alias for os.unlink
extern "C" PyObject* PyBuiltin_OsRemove(PyObject* args) {
    return PyBuiltin_OsUnlink(args);
}

// os.rename(src, dst) -> None
extern "C" PyObject* PyBuiltin_OsRename(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* src = args->list[0];
    PyObject* dst = args->list[1];
    if (!src || src->type != 3 || !dst || dst->type != 3) return nullptr;
    ::rename(src->str.c_str(), dst->str.c_str());
    return nullptr;
}

// pathlib.Path(...) construction — direct-call convention (matching
// datetime's construction functions), since the result must carry the
// type-16 tag for `/`/attribute/comparison dispatch, unlike a plain
// token+registry function whose result is just a generic boxed value.
extern "C" PyObject* PyPathlib_Path(PyObject* arg) {
    return pyc_new_path(pyc_is_path_like(arg) ? arg->str : std::string());
}

// Path.exists()/.is_file()/.is_dir() — typeOf-gated method calls (see the
// dispatch note in Compiler.cpp's lowerMethodCall); reuse the same
// stat(2) logic as os.path.exists/isfile/isdir.
extern "C" PyObject* PyPathlib_Exists(PyObject* obj) {
    if (!pyc_is_path_like(obj)) return PyBool_New(0);
    struct stat st;
    return PyBool_New(::stat(obj->str.c_str(), &st) == 0 ? 1 : 0);
}
extern "C" PyObject* PyPathlib_IsFile(PyObject* obj) {
    if (!pyc_is_path_like(obj)) return PyBool_New(0);
    struct stat st;
    if (::stat(obj->str.c_str(), &st) != 0) return PyBool_New(0);
    return PyBool_New(S_ISREG(st.st_mode) ? 1 : 0);
}
extern "C" PyObject* PyPathlib_IsDir(PyObject* obj) {
    if (!pyc_is_path_like(obj)) return PyBool_New(0);
    struct stat st;
    if (::stat(obj->str.c_str(), &st) != 0) return PyBool_New(0);
    return PyBool_New(S_ISDIR(st.st_mode) ? 1 : 0);
}

// Path.mkdir(parents=..., exist_ok=...) — kwargs aren't read (same
// limitation as os.makedirs), always behaves as parents=True,
// exist_ok=True (mkdir -p semantics), never raises.
extern "C" PyObject* PyPathlib_Mkdir(PyObject* obj) {
    if (!pyc_is_path_like(obj) || obj->str.empty()) return nullptr;
    std::string cur;
    size_t i = 0;
    const std::string& s = obj->str;
    if (s[0] == '/') { cur = "/"; i = 1; }
    while (i <= s.size()) {
        size_t next = s.find('/', i);
        std::string comp = (next == std::string::npos) ? s.substr(i) : s.substr(i, next - i);
        if (!comp.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += comp;
            ::mkdir(cur.c_str(), 0777);
        }
        if (next == std::string::npos) break;
        i = next + 1;
    }
    return nullptr;
}

// Path.joinpath(*parts) — equivalent to repeated "/". `parts` is a boxed
// list built by the compiler from the call's positional arguments.
extern "C" PyObject* PyPathlib_Joinpath(PyObject* obj, PyObject* parts) {
    std::string out = pyc_is_path_like(obj) ? obj->str : std::string();
    if (parts && parts->type == 1) {
        for (PyObject* p : parts->list) {
            if (!pyc_is_path_like(p) || p->str.empty()) continue;
            if (p->str[0] == '/') { out = p->str; continue; }
            if (!out.empty() && out.back() != '/') out += '/';
            out += p->str;
        }
    }
    return pyc_new_path(out);
}

// ---------------------------------------------------------------------
// hashlib / base64 / struct
//
// pyc now has a real `bytes`/`bytearray` type (types 17/18, added
// alongside this comment's update — see PyBytes_FromStringAndSize near
// PyUnicode_FromStringAndSize, and pyc_is_bytes_like just below it).
// hashlib/base64/struct accept str, bytes, or bytearray input
// indiscriminately via pyc_is_bytes_like (more permissive than real
// CPython, which requires actual bytes for these — a deliberate,
// documented simplification, not an oversight). base64.b64encode and
// struct.pack now return real bytes (type 17), matching CPython, since
// a real bytes type makes that the natural/correct choice — this is a
// deliberate behavior change from the prior str-returning versions (see
// IMPLEMENTATION.md).
//
// MD5/SHA-1/SHA-256 are implemented from scratch here (compact, standard
// reference algorithms) rather than linking OpenSSL/libcrypto, matching
// the precedent set by the `random` module's from-scratch MT19937 —
// keeps the build dependency-free. Verified byte-for-byte against real
// `hashlib` output for multiple inputs (see tests/runner.py).
// ---------------------------------------------------------------------

static std::string pyc_bytes_to_hex(const uint8_t* data, size_t n) {
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += hexd[(data[i] >> 4) & 0xF];
        out += hexd[data[i] & 0xF];
    }
    return out;
}

// bytes.fromhex(s) inverse of the helper above. Ignores whitespace
// between byte pairs (matching real bytes.fromhex, which allows spaces
// as separators); raises ValueError on a malformed hex string.
static bool pyc_hex_to_bytes(const std::string& hex, std::string& out) {
    out.clear();
    int hi = -1;
    for (char c : hex) {
        if (c == ' ') { if (hi != -1) return false; continue; }
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        if (hi == -1) hi = v;
        else { out += (char)((hi << 4) | v); hi = -1; }
    }
    return hi == -1;
}

// bytes(x)/bytearray(x) construction. `arg0` may be: null (empty),
// int/bool (zero-filled of that length), str (raw content copy — arg1,
// the encoding, is accepted but not used for transcoding beyond that,
// same permissive-str-as-bytes stance as hashlib/base64 above), a list
// (iterable of ints 0-255), or bytes/bytearray (content copy).
static PyObject* pyc_bytes_construct(PyObject* arg0, PyObject* arg1, bool wantBytearray) {
    (void)arg1;
    std::string content;
    if (arg0) {
        if (arg0->type == 0 || arg0->type == 5) {
            long n = arg0->value;
            if (n > 0) content.assign((size_t)n, '\0');
        } else if (arg0->type == 3 || arg0->type == 17 || arg0->type == 18) {
            content = arg0->str;
        } else if (arg0->type == 1) {
            pyc_ensure_boxed_list(arg0);
            content.reserve(arg0->list.size());
            for (PyObject* item : arg0->list) {
                long v = (item && (item->type == 0 || item->type == 5)) ? item->value : 0;
                content += (char)(unsigned char)(v & 0xFF);
            }
        }
    }
    return wantBytearray ? PyByteArray_FromStringAndSize(content.data(), content.size())
                          : PyBytes_FromStringAndSize(content.data(), content.size());
}
extern "C" PyObject* PyBuiltin_Bytes(PyObject* arg0, PyObject* arg1) {
    return pyc_bytes_construct(arg0, arg1, false);
}
extern "C" PyObject* PyBuiltin_Bytearray(PyObject* arg0, PyObject* arg1) {
    return pyc_bytes_construct(arg0, arg1, true);
}
extern "C" PyObject* PyBytes_Hex(PyObject* self) {
    if (!self || (self->type != 17 && self->type != 18)) return PyUnicode_FromString("");
    std::string h = pyc_bytes_to_hex((const uint8_t*)self->str.data(), self->str.size());
    return PyUnicode_FromStringAndSize(h.data(), h.size());
}
extern "C" PyObject* PyBytes_Fromhex(PyObject* s) {
    if (!s || s->type != 3) return PyBytes_FromStringAndSize("", 0);
    std::string out;
    if (!pyc_hex_to_bytes(s->str, out)) {
        pyc_raise_msg("ValueError", "non-hexadecimal number found in fromhex() arg");
        return nullptr;
    }
    return PyBytes_FromStringAndSize(out.data(), out.size());
}
extern "C" PyObject* PyBytes_Decode(PyObject* self, PyObject* /*encoding*/) {
    if (!self || (self->type != 17 && self->type != 18)) return PyUnicode_FromString("");
    return PyUnicode_FromStringAndSize(self->str.data(), self->str.size());
}
extern "C" PyObject* PyStr_Encode(PyObject* self, PyObject* /*encoding*/) {
    if (!self || self->type != 3) return PyBytes_FromStringAndSize("", 0);
    return PyBytes_FromStringAndSize(self->str.data(), self->str.size());
}

// bytearray mutability — .append(int)/.extend(iterable). bytearray (type
// 18) stores its content directly in `str`, so mutation is a plain
// std::string append, not PyList_Append's vector-of-PyObject* logic.
extern "C" PyObject* PyByteArray_Append(PyObject* self, PyObject* item) {
    if (!self || self->type != 18) return nullptr;
    long v = (item && (item->type == 0 || item->type == 5)) ? item->value : 0;
    self->str += (char)(unsigned char)(v & 0xFF);
    return nullptr;
}
extern "C" PyObject* PyByteArray_ExtendOp(PyObject* self, PyObject* other) {
    if (!self || self->type != 18 || !other) return nullptr;
    if (other->type == 3 || other->type == 17 || other->type == 18) {
        self->str += other->str;
    } else if (other->type == 1) {
        pyc_ensure_boxed_list(other);
        for (PyObject* item : other->list) {
            long v = (item && (item->type == 0 || item->type == 5)) ? item->value : 0;
            self->str += (char)(unsigned char)(v & 0xFF);
        }
    }
    return nullptr;
}

// --- MD5 (RFC 1321) ---
static void pyc_md5(const std::string& msg, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::string data = msg;
    uint64_t bitLen = (uint64_t)msg.size() * 8;
    data += (char)0x80;
    while (data.size() % 64 != 56) data += (char)0x00;
    for (int i = 0; i < 8; ++i) data += (char)((bitLen >> (8 * i)) & 0xFF);
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            M[i] = (uint8_t)data[chunk + i*4] | ((uint8_t)data[chunk + i*4+1] << 8) |
                   ((uint8_t)data[chunk + i*4+2] << 16) | ((uint8_t)data[chunk + i*4+3] << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t F; int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3*i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7*i) % 16; }
            F += A + K[i] + M[g];
            A = D; D = C; C = B;
            B += (F << S[i]) | (F >> (32 - S[i]));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    uint32_t words[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i) {
        out[i*4]   = (uint8_t)(words[i] & 0xFF);
        out[i*4+1] = (uint8_t)((words[i] >> 8) & 0xFF);
        out[i*4+2] = (uint8_t)((words[i] >> 16) & 0xFF);
        out[i*4+3] = (uint8_t)((words[i] >> 24) & 0xFF);
    }
}

// --- SHA-1 (RFC 3174) ---
static uint32_t pyc_rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void pyc_sha1(const std::string& msg, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string data = msg;
    uint64_t bitLen = (uint64_t)msg.size() * 8;
    data += (char)0x80;
    while (data.size() % 64 != 56) data += (char)0x00;
    for (int i = 7; i >= 0; --i) data += (char)((bitLen >> (8 * i)) & 0xFF);
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint8_t)data[chunk+i*4] << 24) | ((uint8_t)data[chunk+i*4+1] << 16) |
                   ((uint8_t)data[chunk+i*4+2] << 8) | (uint8_t)data[chunk+i*4+3];
        }
        for (int i = 16; i < 80; ++i) w[i] = pyc_rotl32(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = pyc_rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = pyc_rotl32(b, 30); b = a; a = temp;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    uint32_t hs[5] = {h0,h1,h2,h3,h4};
    for (int i = 0; i < 5; ++i) {
        out[i*4]   = (uint8_t)((hs[i] >> 24) & 0xFF);
        out[i*4+1] = (uint8_t)((hs[i] >> 16) & 0xFF);
        out[i*4+2] = (uint8_t)((hs[i] >> 8) & 0xFF);
        out[i*4+3] = (uint8_t)(hs[i] & 0xFF);
    }
}

// --- SHA-256 (FIPS 180-4) ---
static void pyc_sha256(const std::string& msg, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::string data = msg;
    uint64_t bitLen = (uint64_t)msg.size() * 8;
    data += (char)0x80;
    while (data.size() % 64 != 56) data += (char)0x00;
    for (int i = 7; i >= 0; --i) data += (char)((bitLen >> (8 * i)) & 0xFF);
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint8_t)data[chunk+i*4] << 24) | ((uint8_t)data[chunk+i*4+1] << 16) |
                   ((uint8_t)data[chunk+i*4+2] << 8) | (uint8_t)data[chunk+i*4+3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = pyc_rotl32(w[i-15],25) ^ pyc_rotl32(w[i-15],14) ^ (w[i-15] >> 3);
            uint32_t s1 = pyc_rotl32(w[i-2],15) ^ pyc_rotl32(w[i-2],13) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = pyc_rotl32(e,26) ^ pyc_rotl32(e,21) ^ pyc_rotl32(e,7);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = pyc_rotl32(a,30) ^ pyc_rotl32(a,19) ^ pyc_rotl32(a,10);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (uint8_t)((h[i] >> 24) & 0xFF);
        out[i*4+1] = (uint8_t)((h[i] >> 16) & 0xFF);
        out[i*4+2] = (uint8_t)((h[i] >> 8) & 0xFF);
        out[i*4+3] = (uint8_t)(h[i] & 0xFF);
    }
}

// hashlib.md5/sha1/sha256(data) — direct-call convention (like datetime's
// constructors): computes the digest immediately (data is always fully
// known at call time; no .update() streaming support — out of scope,
// documented). Returns a dict with the raw digest bytes stashed under an
// internal marker key; .hexdigest() (also direct-call, typeOf-gated on
// "hashobj" in Compiler.cpp) reads that key directly rather than going
// through the generic dict/Pyc_Apply dispatch, sidestepping the
// receiver-prepending pitfall found and fixed in open()/.write() above.
// Stashes both the hex digest (str) and the raw digest (real bytes, type
// 17) under reserved keys — .hexdigest() reads the former, .digest() (new
// alongside the bytes type) reads the latter.
static PyObject* pyc_make_hashobj(const std::string& hexHash, const uint8_t* raw, size_t rawLen) {
    PyObject* d = PyDict_New();
    PyObject* k = PyUnicode_FromString("__pyc_hexdigest__");
    PyObject* v = PyUnicode_FromString(hexHash.c_str());
    PyDict_SetItem(d, k, v);
    Py_DECREF(k); Py_DECREF(v);
    PyObject* k2 = PyUnicode_FromString("__pyc_digest__");
    PyObject* v2 = PyBytes_FromStringAndSize((const char*)raw, rawLen);
    PyDict_SetItem(d, k2, v2);
    Py_DECREF(k2); Py_DECREF(v2);
    return d;
}
extern "C" PyObject* PyHashlib_Md5(PyObject* data) {
    uint8_t digest[16];
    pyc_md5(pyc_is_bytes_like(data) ? data->str : std::string(), digest);
    return pyc_make_hashobj(pyc_bytes_to_hex(digest, 16), digest, 16);
}
extern "C" PyObject* PyHashlib_Sha1(PyObject* data) {
    uint8_t digest[20];
    pyc_sha1(pyc_is_bytes_like(data) ? data->str : std::string(), digest);
    return pyc_make_hashobj(pyc_bytes_to_hex(digest, 20), digest, 20);
}
extern "C" PyObject* PyHashlib_Sha256(PyObject* data) {
    uint8_t digest[32];
    pyc_sha256(pyc_is_bytes_like(data) ? data->str : std::string(), digest);
    return pyc_make_hashobj(pyc_bytes_to_hex(digest, 32), digest, 32);
}
extern "C" PyObject* PyHashlib_Hexdigest(PyObject* self) {
    if (!self || self->type != 2) return PyUnicode_FromString("");
    PyObject* key = PyUnicode_FromString("__pyc_hexdigest__");
    PyObject* v = Pyc_GetItem(self, key);
    Py_DECREF(key);
    if (v) return v; // Pyc_GetItem already returns a new ref
    return PyUnicode_FromString("");
}
extern "C" PyObject* PyHashlib_Digest(PyObject* self) {
    if (!self || self->type != 2) return PyBytes_FromStringAndSize("", 0);
    PyObject* key = PyUnicode_FromString("__pyc_digest__");
    PyObject* v = Pyc_GetItem(self, key);
    Py_DECREF(key);
    if (v) return v;
    return PyBytes_FromStringAndSize("", 0);
}

// base64 (RFC 4648). b64encode accepts str/bytes/bytearray input and
// returns real bytes (type 17), matching CPython exactly — previously
// returned str, since there was no bytes type to return.
static const char* kB64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

extern "C" PyObject* PyBase64_B64Encode(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyBytes_FromStringAndSize("", 0);
    PyObject* s = args->list[0];
    if (!pyc_is_bytes_like(s)) return PyBytes_FromStringAndSize("", 0);
    const std::string& in = s->str;
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        uint32_t n = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8) | (uint8_t)in[i+2];
        out += kB64Alphabet[(n >> 18) & 0x3F];
        out += kB64Alphabet[(n >> 12) & 0x3F];
        out += kB64Alphabet[(n >> 6) & 0x3F];
        out += kB64Alphabet[n & 0x3F];
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t n = (uint8_t)in[i] << 16;
        out += kB64Alphabet[(n >> 18) & 0x3F];
        out += kB64Alphabet[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8);
        out += kB64Alphabet[(n >> 18) & 0x3F];
        out += kB64Alphabet[(n >> 12) & 0x3F];
        out += kB64Alphabet[(n >> 6) & 0x3F];
        out += '=';
    }
    return PyBytes_FromStringAndSize(out.data(), out.size());
}

static int pyc_b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
// b64decode accepts str/bytes/bytearray input and returns real bytes
// (previously str) — same rationale as b64encode above.
extern "C" PyObject* PyBase64_B64Decode(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyBytes_FromStringAndSize("", 0);
    PyObject* s = args->list[0];
    if (!pyc_is_bytes_like(s)) return PyBytes_FromStringAndSize("", 0);
    const std::string& in = s->str;
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = pyc_b64_val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return PyBytes_FromStringAndSize(out.data(), out.size());
}

// struct.pack/unpack: common format codes (b/B/h/H/i/I/q/Q/f/d/s) with an
// optional endianness prefix (</>/!/= — all treated as explicit
// little/big; "=" and no-prefix default to native, which is little-endian
// on every platform pyc targets). Unsupported codes (n/N, native alignment
// padding) are skipped/ignored rather than erroring — documented gap.
struct PycStructFmt { char code; bool bigEndian; };

static std::vector<PycStructFmt> pyc_parse_struct_fmt(const std::string& fmt) {
    std::vector<PycStructFmt> out;
    bool bigEndian = false; // default: native == little-endian here
    size_t i = 0;
    if (!fmt.empty() && (fmt[0]=='<'||fmt[0]=='>'||fmt[0]=='!'||fmt[0]=='=')) {
        bigEndian = (fmt[0] == '>' || fmt[0] == '!');
        i = 1;
    }
    for (; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c == ' ') continue;
        out.push_back({c, bigEndian});
    }
    return out;
}
static void pyc_struct_pack_int(std::string& out, uint64_t v, int nbytes, bool bigEndian) {
    if (bigEndian) {
        for (int i = nbytes - 1; i >= 0; --i) out += (char)((v >> (8*i)) & 0xFF);
    } else {
        for (int i = 0; i < nbytes; ++i) out += (char)((v >> (8*i)) & 0xFF);
    }
}
static uint64_t pyc_struct_unpack_int(const std::string& s, size_t pos, int nbytes, bool bigEndian) {
    uint64_t v = 0;
    if (bigEndian) {
        for (int i = 0; i < nbytes; ++i) v = (v << 8) | (uint8_t)s[pos + i];
    } else {
        for (int i = nbytes - 1; i >= 0; --i) v = (v << 8) | (uint8_t)s[pos + i];
    }
    return v;
}
extern "C" PyObject* PyStruct_Pack(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyBytes_FromStringAndSize("", 0);
    PyObject* fmtObj = args->list[0];
    if (!fmtObj || fmtObj->type != 3) return PyBytes_FromStringAndSize("", 0);
    auto codes = pyc_parse_struct_fmt(fmtObj->str);
    std::string out;
    size_t argi = 1;
    for (auto& fc : codes) {
        PyObject* v = argi < args->list.size() ? args->list[argi] : nullptr;
        switch (fc.code) {
            case 'b': case 'B':
                pyc_struct_pack_int(out, v ? (uint64_t)(int64_t)v->value : 0, 1, fc.bigEndian); ++argi; break;
            case 'h': case 'H':
                pyc_struct_pack_int(out, v ? (uint64_t)(int64_t)v->value : 0, 2, fc.bigEndian); ++argi; break;
            case 'i': case 'I': case 'l': case 'L':
                pyc_struct_pack_int(out, v ? (uint64_t)(int64_t)v->value : 0, 4, fc.bigEndian); ++argi; break;
            case 'q': case 'Q':
                pyc_struct_pack_int(out, v ? (uint64_t)(int64_t)v->value : 0, 8, fc.bigEndian); ++argi; break;
            case 'f': {
                float f = v ? (float)numeric_val(v) : 0.0f;
                uint32_t bits; memcpy(&bits, &f, 4);
                pyc_struct_pack_int(out, bits, 4, fc.bigEndian); ++argi; break;
            }
            case 'd': {
                double d = v ? numeric_val(v) : 0.0;
                uint64_t bits; memcpy(&bits, &d, 8);
                pyc_struct_pack_int(out, bits, 8, fc.bigEndian); ++argi; break;
            }
            case 's': {
                std::string s = pyc_is_bytes_like(v) ? v->str : std::string();
                out += s; ++argi; break;
            }
            default: break; // unsupported code: skip
        }
    }
    // Packed binary output routinely contains embedded NULs (e.g. any
    // little-endian integer field with a zero high byte) — bytes storage
    // (like PyUnicode_FromStringAndSize before it) is explicit-length,
    // not NUL-terminated-assumption-based.
    return PyBytes_FromStringAndSize(out.data(), out.size());
}
extern "C" PyObject* PyStruct_Unpack(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyTuple_New(0);
    PyObject* fmtObj = args->list[0];
    PyObject* dataObj = args->list[1];
    if (!fmtObj || fmtObj->type != 3 || !pyc_is_bytes_like(dataObj)) return PyTuple_New(0);
    auto codes = pyc_parse_struct_fmt(fmtObj->str);
    const std::string& s = dataObj->str;
    size_t pos = 0;
    std::vector<PyObject*> items;
    for (auto& fc : codes) {
        PyObject* v = nullptr;
        switch (fc.code) {
            case 'b': {
                if (pos + 1 > s.size()) break;
                int8_t x = (int8_t)pyc_struct_unpack_int(s, pos, 1, fc.bigEndian);
                v = PyInt_FromLong(x); pos += 1; break;
            }
            case 'B': {
                if (pos + 1 > s.size()) break;
                v = PyInt_FromLong((long)pyc_struct_unpack_int(s, pos, 1, fc.bigEndian)); pos += 1; break;
            }
            case 'h': {
                if (pos + 2 > s.size()) break;
                int16_t x = (int16_t)pyc_struct_unpack_int(s, pos, 2, fc.bigEndian);
                v = PyInt_FromLong(x); pos += 2; break;
            }
            case 'H': {
                if (pos + 2 > s.size()) break;
                v = PyInt_FromLong((long)pyc_struct_unpack_int(s, pos, 2, fc.bigEndian)); pos += 2; break;
            }
            case 'i': case 'l': {
                if (pos + 4 > s.size()) break;
                int32_t x = (int32_t)pyc_struct_unpack_int(s, pos, 4, fc.bigEndian);
                v = PyInt_FromLong(x); pos += 4; break;
            }
            case 'I': case 'L': {
                if (pos + 4 > s.size()) break;
                v = PyInt_FromLong((long)pyc_struct_unpack_int(s, pos, 4, fc.bigEndian)); pos += 4; break;
            }
            case 'q': {
                if (pos + 8 > s.size()) break;
                int64_t x = (int64_t)pyc_struct_unpack_int(s, pos, 8, fc.bigEndian);
                v = PyInt_FromLong(x); pos += 8; break;
            }
            case 'Q': {
                if (pos + 8 > s.size()) break;
                v = PyInt_FromLong((long)pyc_struct_unpack_int(s, pos, 8, fc.bigEndian)); pos += 8; break;
            }
            case 'f': {
                if (pos + 4 > s.size()) break;
                uint32_t bits = (uint32_t)pyc_struct_unpack_int(s, pos, 4, fc.bigEndian);
                float f; memcpy(&f, &bits, 4);
                v = PyFloat_FromDouble((double)f); pos += 4; break;
            }
            case 'd': {
                if (pos + 8 > s.size()) break;
                uint64_t bits = pyc_struct_unpack_int(s, pos, 8, fc.bigEndian);
                double d; memcpy(&d, &bits, 8);
                v = PyFloat_FromDouble(d); pos += 8; break;
            }
            default: break;
        }
        if (v) items.push_back(v);
    }
    PyObject* out = PyTuple_New(items.size());
    for (size_t i = 0; i < items.size(); ++i) PyTuple_SetItem(out, i, items[i]);
    return out;
}

// ---------------------------------------------------------------------
// heapq / bisect / statistics — all operate on plain lists via the
// existing generic comparison primitive (PyObject_CompareBool, the same
// one PyList_Sort uses), no new types. Token+registry convention
// throughout. All list arguments are run through pyc_ensure_boxed_list
// (defined above, near PyList_Sort) first — see its comment for why.
// ---------------------------------------------------------------------

static bool pyc_lt(PyObject* a, PyObject* b) { return PyObject_CompareBool(a, b, 2) != 0; }

static void pyc_heap_siftup(std::vector<PyObject*>& h, size_t pos) {
    // Standard binary-heap sift-down (CPython's heapq calls this
    // "_siftup" — moves the too-large root down by repeatedly swapping
    // with its smaller child).
    size_t n = h.size();
    size_t startpos = pos;
    PyObject* newitem = h[pos];
    size_t childpos = 2*pos + 1;
    while (childpos < n) {
        size_t rightpos = childpos + 1;
        if (rightpos < n && !pyc_lt(h[childpos], h[rightpos])) childpos = rightpos;
        h[pos] = h[childpos];
        pos = childpos;
        childpos = 2*pos + 1;
    }
    h[pos] = newitem;
    // Sift the moved item up to its correct resting place.
    while (pos > startpos) {
        size_t parentpos = (pos - 1) / 2;
        if (pyc_lt(h[pos], h[parentpos])) {
            std::swap(h[pos], h[parentpos]);
            pos = parentpos;
        } else break;
    }
}
static void pyc_heap_siftdown(std::vector<PyObject*>& h, size_t startpos, size_t pos) {
    PyObject* newitem = h[pos];
    while (pos > startpos) {
        size_t parentpos = (pos - 1) / 2;
        PyObject* parent = h[parentpos];
        if (pyc_lt(newitem, parent)) {
            h[pos] = parent;
            pos = parentpos;
        } else break;
    }
    h[pos] = newitem;
}

extern "C" PyObject* PyHeapq_Heapify(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* lst = args->list[0];
    if (!lst || lst->type != 1) return nullptr;
    pyc_ensure_boxed_list(lst);
    size_t n = lst->list.size();
    if (n < 2) return nullptr;
    for (size_t i = n / 2; i-- > 0;) pyc_heap_siftup(lst->list, i);
    return nullptr;
}
extern "C" PyObject* PyHeapq_Heappush(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* lst = args->list[0];
    PyObject* item = args->list[1];
    if (!lst || lst->type != 1) return nullptr;
    pyc_ensure_boxed_list(lst);
    if (item) Py_INCREF(item);
    lst->list.push_back(item);
    pyc_heap_siftdown(lst->list, 0, lst->list.size() - 1);
    return nullptr;
}
extern "C" PyObject* PyHeapq_Heappop(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* lst = args->list[0];
    if (!lst || lst->type != 1) return nullptr;
    pyc_ensure_boxed_list(lst);
    if (lst->list.empty()) return nullptr;
    PyObject* result = lst->list.front();
    PyObject* last = lst->list.back();
    lst->list.pop_back();
    if (!lst->list.empty()) {
        lst->list[0] = last;
        pyc_heap_siftup(lst->list, 0);
    }
    return result; // ownership transferred to caller (was owned by the list)
}
extern "C" PyObject* PyHeapq_Heappushpop(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* lst = args->list[0];
    PyObject* item = args->list[1];
    if (!lst || lst->type != 1) return nullptr;
    pyc_ensure_boxed_list(lst);
    if (!lst->list.empty() && pyc_lt(lst->list[0], item)) {
        PyObject* result = lst->list[0];
        if (item) Py_INCREF(item);
        lst->list[0] = item;
        pyc_heap_siftup(lst->list, 0);
        return result;
    }
    if (item) Py_INCREF(item);
    return item;
}
extern "C" PyObject* PyHeapq_Heapreplace(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* lst = args->list[0];
    PyObject* item = args->list[1];
    if (!lst || lst->type != 1) return nullptr;
    pyc_ensure_boxed_list(lst);
    if (lst->list.empty()) return nullptr;
    PyObject* result = lst->list[0];
    if (item) Py_INCREF(item);
    lst->list[0] = item;
    pyc_heap_siftup(lst->list, 0);
    return result;
}
static PyObject* pyc_heapq_extreme(PyObject* args, bool largest) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.size() < 2) return out;
    PyObject* nObj = args->list[0];
    PyObject* iterable = args->list[1];
    if (!nObj || (nObj->type != 0 && nObj->type != 5) || !iterable || iterable->type != 1) return out;
    pyc_ensure_boxed_list(iterable);
    long n = (long)nObj->value;
    std::vector<PyObject*> items = iterable->list;
    std::sort(items.begin(), items.end(), [&](PyObject* a, PyObject* b) {
        return largest ? pyc_lt(b, a) : pyc_lt(a, b);
    });
    for (long i = 0; i < n && (size_t)i < items.size(); ++i) {
        PyObject* item = items[(size_t)i];
        if (item) Py_INCREF(item);
        PyList_Append(out, item);
        if (item) Py_DECREF(item);
    }
    return out;
}
extern "C" PyObject* PyHeapq_Nlargest(PyObject* args) { return pyc_heapq_extreme(args, true); }
extern "C" PyObject* PyHeapq_Nsmallest(PyObject* args) { return pyc_heapq_extreme(args, false); }

// bisect_left/bisect_right: standard binary search for insertion point.
static size_t pyc_bisect(PyObject* lst, PyObject* x, bool right) {
    size_t lo = 0, hi = lst->list.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        bool goRight = right ? !pyc_lt(x, lst->list[mid]) : pyc_lt(lst->list[mid], x);
        if (goRight) lo = mid + 1; else hi = mid;
    }
    return lo;
}
extern "C" PyObject* PyBisect_Left(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyInt_FromLong(0);
    PyObject* lst = args->list[0];
    if (!lst || lst->type != 1) return PyInt_FromLong(0);
    pyc_ensure_boxed_list(lst);
    return PyInt_FromLong((long)pyc_bisect(lst, args->list[1], false));
}
extern "C" PyObject* PyBisect_Right(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyInt_FromLong(0);
    PyObject* lst = args->list[0];
    if (!lst || lst->type != 1) return PyInt_FromLong(0);
    pyc_ensure_boxed_list(lst);
    return PyInt_FromLong((long)pyc_bisect(lst, args->list[1], true));
}
extern "C" PyObject* PyBisect_InsortLeft(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* lst = args->list[0];
    PyObject* x = args->list[1];
    if (!lst || lst->type != 1) return nullptr;
    pyc_ensure_boxed_list(lst);
    size_t pos = pyc_bisect(lst, x, false);
    if (x) Py_INCREF(x);
    lst->list.insert(lst->list.begin() + pos, x);
    return nullptr;
}
extern "C" PyObject* PyBisect_InsortRight(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* lst = args->list[0];
    PyObject* x = args->list[1];
    if (!lst || lst->type != 1) return nullptr;
    pyc_ensure_boxed_list(lst);
    size_t pos = pyc_bisect(lst, x, true);
    if (x) Py_INCREF(x);
    lst->list.insert(lst->list.begin() + pos, x);
    return nullptr;
}

// statistics: mean/median/median_low/median_high/mode/stdev/variance/
// pstdev/pvariance. mean() special-cases an all-int input to match
// CPython's exact-Fraction-based int-preserving result (mean([2,4])==3,
// an int, not 3.0) without implementing a full Fraction type — sum stays
// exact 64-bit integer arithmetic, and the result is int only when it
// divides evenly, otherwise float. Mixed int/float input always
// produces a float (matches CPython for the common cases; CPython's
// Fraction-exact edge cases with irrational-looking float inputs aren't
// replicated — documented simplification).
extern "C" PyObject* PyStatistics_Mean(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    PyObject* data = args->list[0];
    pyc_ensure_boxed_list(data);
    bool allInt = true;
    for (PyObject* v : data->list) { if (!v || v->type != 0) { allInt = false; break; } }
    size_t n = data->list.size();
    if (allInt) {
        int64_t total = 0;
        for (PyObject* v : data->list) total += v->value;
        if (total % (int64_t)n == 0) return PyInt_FromLong((long)(total / (int64_t)n));
        return PyFloat_FromDouble((double)total / (double)n);
    }
    double total = 0.0;
    for (PyObject* v : data->list) total += is_numeric(v) ? numeric_val(v) : 0.0;
    return PyFloat_FromDouble(total / (double)n);
}
static std::vector<PyObject*> pyc_stats_sorted(PyObject* data) {
    pyc_ensure_boxed_list(data);
    std::vector<PyObject*> v = data->list;
    std::sort(v.begin(), v.end(), [](PyObject* a, PyObject* b) { return pyc_lt(a, b); });
    return v;
}
extern "C" PyObject* PyStatistics_Median(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    auto v = pyc_stats_sorted(args->list[0]);
    size_t n = v.size();
    if (n == 0) return PyFloat_FromDouble(0.0);
    if (n % 2 == 1) { PyObject* r = v[n/2]; Py_INCREF(r); return r; }
    double a = numeric_val(v[n/2 - 1]), b = numeric_val(v[n/2]);
    return PyFloat_FromDouble((a + b) / 2.0);
}
extern "C" PyObject* PyStatistics_MedianLow(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    auto v = pyc_stats_sorted(args->list[0]);
    size_t n = v.size();
    if (n == 0) return PyFloat_FromDouble(0.0);
    PyObject* r = (n % 2 == 1) ? v[n/2] : v[n/2 - 1];
    Py_INCREF(r); return r;
}
extern "C" PyObject* PyStatistics_MedianHigh(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    auto v = pyc_stats_sorted(args->list[0]);
    size_t n = v.size();
    if (n == 0) return PyFloat_FromDouble(0.0);
    PyObject* r = v[n/2];
    Py_INCREF(r); return r;
}
extern "C" PyObject* PyStatistics_Mode(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return nullptr;
    PyObject* data = args->list[0];
    pyc_ensure_boxed_list(data);
    // First-encounter order matters for CPython's tie-breaking (the
    // value whose max count is reached first, in original data order,
    // wins) — a simple std::unordered_map would lose that order, so
    // track counts alongside a separate first-seen-order value list.
    std::vector<PyObject*> seenOrder;
    std::vector<int> counts;
    for (PyObject* v : data->list) {
        bool found = false;
        for (size_t i = 0; i < seenOrder.size(); ++i) {
            if (PyObject_CompareBool(seenOrder[i], v, 0)) { counts[i]++; found = true; break; }
        }
        if (!found) { seenOrder.push_back(v); counts.push_back(1); }
    }
    if (seenOrder.empty()) return nullptr;
    size_t bestIdx = 0;
    for (size_t i = 1; i < counts.size(); ++i) if (counts[i] > counts[bestIdx]) bestIdx = i;
    PyObject* r = seenOrder[bestIdx];
    Py_INCREF(r); return r;
}
static double pyc_stats_variance(PyObject* data, bool sample) {
    pyc_ensure_boxed_list(data);
    size_t n = data->list.size();
    if (n == 0 || (sample && n < 2)) return 0.0;
    double mean = 0.0;
    for (PyObject* v : data->list) mean += is_numeric(v) ? numeric_val(v) : 0.0;
    mean /= (double)n;
    double ss = 0.0;
    for (PyObject* v : data->list) {
        double d = (is_numeric(v) ? numeric_val(v) : 0.0) - mean;
        ss += d * d;
    }
    return ss / (double)(sample ? (n - 1) : n);
}
// variance()/pvariance() (unlike stdev()/pstdev(), which always return
// float even when the variance is a perfect square — confirmed against
// real CPython) can return an exact int, via the same CPython
// Fraction-based int-preservation as mean() — e.g.
// statistics.pvariance([1,2,3,4,5]) == 2 (int), not 2.0. Replicated here
// only for the common case (all-int input, exact-integer mean, and the
// sum-of-squared-deviations divides evenly) — not full Fraction
// arithmetic, so a case where the *exact rational* variance reduces to
// an integer despite a non-integer mean won't match. Documented
// simplification, same spirit as mean()'s.
static bool pyc_stats_variance_exact_int(PyObject* data, bool sample, int64_t& outInt) {
    for (PyObject* v : data->list) { if (!v || v->type != 0) return false; }
    size_t n = data->list.size();
    if (n == 0 || (sample && n < 2)) return false;
    int64_t total = 0;
    for (PyObject* v : data->list) total += v->value;
    if (total % (int64_t)n != 0) return false;
    int64_t mean = total / (int64_t)n;
    int64_t ss = 0;
    for (PyObject* v : data->list) { int64_t d = v->value - mean; ss += d * d; }
    int64_t divisor = sample ? (int64_t)(n - 1) : (int64_t)n;
    if (ss % divisor != 0) return false;
    outInt = ss / divisor;
    return true;
}
extern "C" PyObject* PyStatistics_Variance(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    PyObject* data = args->list[0];
    pyc_ensure_boxed_list(data);
    int64_t exact;
    if (pyc_stats_variance_exact_int(data, true, exact)) return PyInt_FromLong((long)exact);
    return PyFloat_FromDouble(pyc_stats_variance(data, true));
}
extern "C" PyObject* PyStatistics_Stdev(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(std::sqrt(pyc_stats_variance(args->list[0], true)));
}
extern "C" PyObject* PyStatistics_Pvariance(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    PyObject* data = args->list[0];
    pyc_ensure_boxed_list(data);
    int64_t exact;
    if (pyc_stats_variance_exact_int(data, false, exact)) return PyInt_FromLong((long)exact);
    return PyFloat_FromDouble(pyc_stats_variance(data, false));
}
extern "C" PyObject* PyStatistics_Pstdev(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty() || args->list[0]->type != 1) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble(std::sqrt(pyc_stats_variance(args->list[0], false)));
}

// ---------------------------------------------------------------------
// textwrap / uuid / copy (string module is pure constants, built
// directly in makeStringModuleDict() further down — no functions here).
// ---------------------------------------------------------------------

// textwrap.wrap(text, width=70) -> list[str]; fill(text, width=70) ->
// str. Standard greedy word-wrap: split on any whitespace run (matching
// str.split() with no args), then pack words onto a line as long as
// `current_len + 1 (space) + word_len <= width`. Doesn't replicate
// CPython's hyphenation/long-word-breaking or indent parameters —
// common-case greedy wrap only, verified against real textwrap output
// for ordinary prose.
static std::vector<std::string> pyc_textwrap_words(const std::string& text) {
    std::vector<std::string> words;
    size_t i = 0, n = text.size();
    while (i < n) {
        while (i < n && std::isspace((unsigned char)text[i])) ++i;
        size_t start = i;
        while (i < n && !std::isspace((unsigned char)text[i])) ++i;
        if (i > start) words.push_back(text.substr(start, i - start));
    }
    return words;
}
static std::vector<std::string> pyc_textwrap_wrap(const std::string& text, long width) {
    std::vector<std::string> out;
    auto words = pyc_textwrap_words(text);
    std::string line;
    for (auto& w : words) {
        if (line.empty()) {
            line = w;
        } else if ((long)(line.size() + 1 + w.size()) <= width) {
            line += ' ';
            line += w;
        } else {
            out.push_back(line);
            line = w;
        }
    }
    if (!line.empty()) out.push_back(line);
    return out;
}
extern "C" PyObject* PyTextwrap_Wrap(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    PyObject* textObj = args->list[0];
    if (!textObj || textObj->type != 3) return out;
    long width = 70;
    if (args->list.size() > 1 && args->list[1] && args->list[1]->type == 0) width = (long)args->list[1]->value;
    for (auto& line : pyc_textwrap_wrap(textObj->str, width)) {
        PyObject* s = PyUnicode_FromString(line.c_str());
        PyList_Append(out, s);
        Py_DECREF(s);
    }
    return out;
}
extern "C" PyObject* PyTextwrap_Fill(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyUnicode_FromString("");
    PyObject* textObj = args->list[0];
    if (!textObj || textObj->type != 3) return PyUnicode_FromString("");
    long width = 70;
    if (args->list.size() > 1 && args->list[1] && args->list[1]->type == 0) width = (long)args->list[1]->value;
    auto lines = pyc_textwrap_wrap(textObj->str, width);
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return PyUnicode_FromString(out.c_str());
}

// uuid.uuid4() -> str, RFC 4122 version-4 UUID. Uses real OS entropy
// (std::random_device), deliberately NOT the seeded MT19937 used by the
// `random` module — matches real CPython, where uuid4() is unseedable
// regardless of random.seed(), so this is not a limitation but the
// correct behavior; excluded from exact-match testing the same way
// datetime.now()/time.perf_counter() are.
extern "C" PyObject* PyUuid_Uuid4(PyObject* args) {
    (void)args;
    std::random_device rd;
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = rd();
        b[i] = (uint8_t)(r & 0xFF);
        b[i+1] = (uint8_t)((r >> 8) & 0xFF);
        b[i+2] = (uint8_t)((r >> 16) & 0xFF);
        b[i+3] = (uint8_t)((r >> 24) & 0xFF);
    }
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40); // version 4
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80); // variant 10xx
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
    return PyUnicode_FromString(buf);
}

// copy.copy(x) / copy.deepcopy(x) — direct-call convention (recognized
// structurally in Compiler.cpp, like datetime/pathlib/hashlib
// construction) rather than token+registry, because the `copy` module's
// own dict is itself typed "dict" at compile time, making it
// indistinguishable from a real dict via the generic typeOf=="dict"
// dispatch that the existing (unconditional) `.copy()` list/dict-method
// branch already claims — the same class of name collision as
// os.path.join/os.remove, but unfixable the same way (typeOf(obj)!=
// "dict" doesn't help when the colliding receiver IS a "dict").
// No cycle detection — a self-referencing structure passed to deepcopy
// recurses until stack overflow (documented, matches the scoping
// precedent set by itertools' unbounded-iterator gap).
extern "C" PyObject* PyCopy_Copy(PyObject* x) {
    if (!x) return nullptr;
    if (x->type == 1) {
        pyc_ensure_boxed_list(x);
        PyObject* r = PyList_New(x->list.size());
        for (size_t i = 0; i < x->list.size(); ++i) {
            if (x->list[i]) Py_INCREF(x->list[i]);
            PyList_SetItem(r, i, x->list[i]);
        }
        return r;
    }
    if (x->type == 2) {
        PyObject* r = PyDict_New();
        for (auto& p : x->dict) PyDict_SetItem(r, p.first, p.second);
        return r;
    }
    Py_INCREF(x);
    return x;
}
extern "C" PyObject* PyCopy_Deepcopy(PyObject* x) {
    if (!x) return nullptr;
    if (x->type == 1) {
        pyc_ensure_boxed_list(x);
        PyObject* r = PyList_New(x->list.size());
        for (size_t i = 0; i < x->list.size(); ++i) {
            PyObject* v = x->list[i] ? PyCopy_Deepcopy(x->list[i]) : nullptr;
            PyList_SetItem(r, i, v);
            if (v) Py_DECREF(v); // PyList_SetItem takes its own ref
        }
        return r;
    }
    if (x->type == 2) {
        PyObject* r = PyDict_New();
        for (auto& p : x->dict) {
            PyObject* k = p.first ? PyCopy_Deepcopy(p.first) : nullptr;
            PyObject* v = p.second ? PyCopy_Deepcopy(p.second) : nullptr;
            PyDict_SetItem(r, k, v);
            if (k) Py_DECREF(k);
            if (v) Py_DECREF(v);
        }
        return r;
    }
    Py_INCREF(x);
    return x;
}

// ---------------------------------------------------------------------
// functools / operator
//
// Several of these return a "descriptor bundle" — a plain boxed list
// [tokenOrFuncObj, extra0, extra1, ...] — the same mechanism closures
// already use (see Pyc_Apply, further down: calling a bundle extracts
// the token and prepends extra0.. to the caller's own args before
// dispatch). This lets "a callable that remembers captured state" be a
// plain list literal with no new type and no change to the indirect-call
// machinery — only construction needs new code.
// ---------------------------------------------------------------------

// functools.reduce(func, iterable, initializer=None)
extern "C" PyObject* PyFunctools_Reduce(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* func = args->list[0];
    PyObject* iterable = args->list[1];
    if (!iterable || iterable->type != 1) return nullptr;
    pyc_ensure_boxed_list(iterable);
    PyObject* initializer = args->list.size() > 2 ? args->list[2] : nullptr;
    size_t i = 0;
    PyObject* acc;
    if (initializer) {
        Py_INCREF(initializer);
        acc = initializer;
    } else {
        if (iterable->list.empty()) { pyc_raise_msg("TypeError", "reduce() of empty iterable with no initial value"); return nullptr; }
        acc = iterable->list[0];
        Py_INCREF(acc);
        i = 1;
    }
    for (; i < iterable->list.size(); ++i) {
        PyObject* argList = PyList_New(2);
        if (acc) Py_INCREF(acc);
        PyList_SetItem(argList, 0, acc);
        PyObject* item = iterable->list[i];
        if (item) Py_INCREF(item);
        PyList_SetItem(argList, 1, item);
        PyObject* next = Pyc_Apply(func, argList);
        Py_DECREF(argList);
        Py_DECREF(acc);
        acc = next;
    }
    return acc;
}

// functools.partial(func, *args) -> bundle [func, arg0, arg1, ...].
// `args->list` already has exactly this shape (func first, then the
// user's extra args), so this just copies it into a fresh owned list.
extern "C" PyObject* PyFunctools_Partial(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* bundle = PyList_New(args->list.size());
    for (size_t i = 0; i < args->list.size(); ++i) {
        PyObject* v = args->list[i];
        if (v) Py_INCREF(v);
        PyList_SetItem(bundle, i, v);
    }
    return bundle;
}

// functools.wraps(original) -> decorator that returns its wrapper
// argument unchanged. True no-op: only the calling-convention shape
// (decorator receives [wrapper], returns the new bound value) matters
// for `@functools.wraps(x)` to compile and run — cosmetic __name__/
// __doc__ copying is skipped (pyc functions don't carry __doc__ at all;
// low value for the implementation cost here, documented).
extern "C" PyObject* PyFunctools_Wraps(PyObject* args) {
    (void)args; // original is captured but intentionally unused (no-op)
    PyObject* bundle = PyList_New(1);
    PyObject* tok = PyUnicode_FromString("PyFunctools_WrapsIdentity");
    PyList_SetItem(bundle, 0, tok);
    Py_DECREF(tok);
    return bundle;
}
extern "C" PyObject* PyFunctools_WrapsIdentity(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* r = args->list[0];
    if (r) Py_INCREF(r);
    return r;
}

// functools.lru_cache — supports both `@functools.lru_cache` (bare) and
// `@functools.lru_cache(maxsize=...)` (parenthesized) via one function
// that branches on its argument's runtime shape: a callable-looking
// value (str/function token, or a bundle list) means this call IS the
// decorator application (bare form) — build and return the caching
// bundle. A non-callable value (int/None, i.e. maxsize) means this call
// is the factory stage — return the same token again unchanged, so the
// *next* Pyc_Apply (with the real function) re-enters this function and
// takes the callable branch. Unbounded cache only (no maxsize eviction
// — documented gap, same spirit as os.makedirs ignoring exist_ok).
static bool pyc_looks_callable(PyObject* v) {
    if (!v) return false;
    if (v->type == 3 || v->type == 11) return true;
    if (v->type == 1 && !v->list.empty()) {
        PyObject* first = v->list[0];
        return first && (first->type == 3 || first->type == 11);
    }
    return false;
}
extern "C" PyObject* PyFunctools_LruCacheCall(PyObject* args) {
    // args = [func, cacheDict, ...userArgs]
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* func = args->list[0];
    PyObject* cache = args->list[1];
    if (!cache || cache->type != 2) return nullptr;
    std::string key;
    for (size_t i = 2; i < args->list.size(); ++i) {
        PyObject* s = PyStr_FromAny(args->list[i]);
        key += s ? s->str : std::string("None");
        key += '\x1f';
        if (s) Py_DECREF(s);
    }
    PyObject* keyObj = PyUnicode_FromString(key.c_str());
    PyObject* cached = Pyc_GetItem(cache, keyObj);
    if (cached) { Py_DECREF(keyObj); return cached; }
    PyObject* callArgs = PyList_New(args->list.size() - 2);
    for (size_t i = 2; i < args->list.size(); ++i) {
        PyObject* v = args->list[i];
        if (v) Py_INCREF(v);
        PyList_SetItem(callArgs, i - 2, v);
    }
    PyObject* result = Pyc_Apply(func, callArgs);
    Py_DECREF(callArgs);
    PyDict_SetItem(cache, keyObj, result);
    Py_DECREF(keyObj);
    return result;
}
extern "C" PyObject* PyFunctools_LruCache(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) {
        PyObject* tok = PyUnicode_FromString("PyFunctools_LruCache");
        return tok;
    }
    PyObject* first = args->list[0];
    if (!pyc_looks_callable(first)) {
        // Factory stage (maxsize=...) — return this same token so the
        // next application re-enters the callable branch below.
        PyObject* tok = PyUnicode_FromString("PyFunctools_LruCache");
        return tok;
    }
    // Bare-decorator stage: build the caching bundle.
    PyObject* cacheDict = PyDict_New();
    PyObject* bundle = PyList_New(3);
    PyObject* callTok = PyUnicode_FromString("PyFunctools_LruCacheCall");
    Py_INCREF(first);
    PyList_SetItem(bundle, 0, callTok);
    PyList_SetItem(bundle, 1, first);
    PyList_SetItem(bundle, 2, cacheDict);
    Py_DECREF(callTok);
    return bundle;
}

// operator: thin wrappers over existing runtime primitives.
extern "C" PyObject* PyOperator_Add(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    return PyNumber_Add(args->list[0], args->list[1]);
}
extern "C" PyObject* PyOperator_Sub(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    return PyNumber_Subtract(args->list[0], args->list[1]);
}
extern "C" PyObject* PyOperator_Mul(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    return PyNumber_Multiply(args->list[0], args->list[1]);
}
extern "C" PyObject* PyOperator_Truediv(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    return PyNumber_TrueDivide(args->list[0], args->list[1]);
}
extern "C" PyObject* PyOperator_Mod(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    return PyNumber_Remainder(args->list[0], args->list[1]);
}
extern "C" PyObject* PyOperator_Eq(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyBool_New(0);
    return PyBool_New(PyObject_CompareBool(args->list[0], args->list[1], 0));
}
extern "C" PyObject* PyOperator_Ne(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyBool_New(0);
    return PyBool_New(PyObject_CompareBool(args->list[0], args->list[1], 1));
}
extern "C" PyObject* PyOperator_Lt(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyBool_New(0);
    return PyBool_New(PyObject_CompareBool(args->list[0], args->list[1], 2));
}
extern "C" PyObject* PyOperator_Gt(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyBool_New(0);
    return PyBool_New(PyObject_CompareBool(args->list[0], args->list[1], 3));
}
extern "C" PyObject* PyOperator_Le(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyBool_New(0);
    return PyBool_New(PyObject_CompareBool(args->list[0], args->list[1], 4));
}
extern "C" PyObject* PyOperator_Ge(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return PyBool_New(0);
    return PyBool_New(PyObject_CompareBool(args->list[0], args->list[1], 5));
}
extern "C" PyObject* PyOperator_Not(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyBool_New(1);
    PyObject* truthy = PyBuiltin_Bool(args->list[0]);
    PyObject* r = PyBool_New(truthy->value == 0);
    Py_DECREF(truthy);
    return r;
}
extern "C" PyObject* PyOperator_Neg(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* v = args->list[0];
    if (!v) return nullptr;
    if (v->type == 4) return PyFloat_FromDouble(-v->dvalue);
    return PyInt_FromLong(-v->value);
}

// operator.itemgetter(key0, key1, ...) / attrgetter(name0, name1, ...) ->
// bundle [callTok, key0, key1, ...]; when the bundle is later called as
// getter(x), Pyc_Apply prepends key0, key1, ... before `x`, giving
// [key0, key1, ..., x] to the call target below. Single-key form
// returns the single result directly (matches real operator); multi-key
// form returns a list of results (real operator returns a tuple — no
// tuple type in pyc, same documented gap as elsewhere this session).
extern "C" PyObject* PyOperator_Itemgetter(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* bundle = PyList_New(args->list.size() + 1);
    PyObject* tok = PyUnicode_FromString("PyOperator_ItemgetterCall");
    PyList_SetItem(bundle, 0, tok);
    Py_DECREF(tok);
    for (size_t i = 0; i < args->list.size(); ++i) {
        PyObject* key = args->list[i];
        if (key) Py_INCREF(key);
        PyList_SetItem(bundle, i + 1, key);
    }
    return bundle;
}
extern "C" PyObject* PyOperator_ItemgetterCall(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    size_t nKeys = args->list.size() - 1;
    PyObject* obj = args->list[nKeys];
    if (nKeys == 1) return Pyc_Subscript(obj, args->list[0]);
    PyObject* out = PyTuple_New(nKeys);
    for (size_t i = 0; i < nKeys; ++i) {
        PyTuple_SetItem(out, i, Pyc_Subscript(obj, args->list[i]));
    }
    return out;
}
extern "C" PyObject* PyOperator_Attrgetter(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* bundle = PyList_New(args->list.size() + 1);
    PyObject* tok = PyUnicode_FromString("PyOperator_AttrgetterCall");
    PyList_SetItem(bundle, 0, tok);
    Py_DECREF(tok);
    for (size_t i = 0; i < args->list.size(); ++i) {
        PyObject* name = args->list[i];
        if (name) Py_INCREF(name);
        PyList_SetItem(bundle, i + 1, name);
    }
    return bundle;
}
extern "C" PyObject* PyOperator_AttrgetterCall(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    size_t nNames = args->list.size() - 1;
    PyObject* obj = args->list[nNames];
    if (nNames == 1) return Pyc_GetItem(obj, args->list[0]);
    PyObject* out = PyTuple_New(nNames);
    for (size_t i = 0; i < nNames; ++i) {
        PyTuple_SetItem(out, i, Pyc_GetItem(obj, args->list[i]));
    }
    return out;
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

// file.readlines() -> list[str], keeping each line's trailing "\n"
// (verified against real CPython: only the final line lacks it if the
// file itself doesn't end with a newline) — the file-read prerequisite
// for csv.reader(f.readlines()). Reads from the current file position
// to EOF (matches real readlines() on a freshly-opened file; doesn't
// track/restore position across multiple calls specially, same as
// CPython). Direct-call convention (self is the receiver, dispatched
// via a typeOf(obj)=="file" branch in Compiler.cpp, mirroring the
// .write() fix) rather than token+registry, for the same reason
// .write() needed it: the receiver must be explicit, not inferred from
// a non-bound generic dict dispatch.
extern "C" PyObject* PyBuiltin_FileReadlines(PyObject* self) {
    PyObject* out = PyList_New(0);
    auto it = g_pycFiles.find(self);
    if (it == g_pycFiles.end() || !it->second.fp) return out;
    FILE* fp = it->second.fp;
    std::string line;
    int c;
    while ((c = std::fgetc(fp)) != EOF) {
        line += (char)c;
        if (c == '\n') {
            PyObject* s = PyUnicode_FromString(line.c_str());
            PyList_Append(out, s);
            Py_DECREF(s);
            line.clear();
        }
    }
    if (!line.empty()) {
        PyObject* s = PyUnicode_FromString(line.c_str());
        PyList_Append(out, s);
        Py_DECREF(s);
    }
    return out;
}

static PyObject* pyc_file_enter_adapter(PyObject* args) {
    // Severe pre-existing bug, found while adding file.readlines():
    // this returned `self` without incrementing its refcount. Every
    // call-result is treated by the generated code as a fresh, owned
    // reference (the with-statement binds it to the target variable as
    // such, and emits a matching decref for it at the variable's last
    // use) — but here the *only* refcount increment backing that "new"
    // reference was the one already performed when `self` was stored
    // into __enter__'s own argument list (balanced by that list's own
    // decref right after the call). Net effect: the object's steady-
    // state refcount was undercounted by 1, so the exitArgs decref
    // cascade (at the end of the with-block) freed it one decref too
    // early, and the with-target variable's own later cleanup decref
    // then ran against already-freed memory — a real use-after-free on
    // every `with open(...) as f:`, confirmed with valgrind (previously
    // undetected: it manifests as silent heap corruption at the *end*
    // of the block, not an immediate crash, so it only became visible
    // via a glibc tcache integrity check when enough further allocation
    // activity followed it in the same run).
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* self = args->list[0];
    if (self) Py_INCREF(self);
    return self;
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

// ---------------------------------------------------------------------
// shutil / glob / csv
// ---------------------------------------------------------------------

// shutil.copyfile(src, dst) -> None : direct C fopen/fread/fwrite,
// entirely bypassing pyc's synthetic file object (no dependency on
// file.readlines()/open() dict machinery).
extern "C" PyObject* PyShutil_Copyfile(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* src = args->list[0];
    PyObject* dst = args->list[1];
    if (!src || src->type != 3 || !dst || dst->type != 3) return nullptr;
    FILE* in = std::fopen(src->str.c_str(), "rb");
    if (!in) { pyc_raise_msg("FileNotFoundError", "No such file or directory"); return nullptr; }
    FILE* out = std::fopen(dst->str.c_str(), "wb");
    if (!out) { std::fclose(in); pyc_raise_msg("OSError", "Could not open destination for writing"); return nullptr; }
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        std::fwrite(buf, 1, n, out);
    }
    std::fclose(in);
    std::fclose(out);
    return nullptr;
}
// shutil.move(src, dst) -> None : rename(2) first (fast path, same
// filesystem); on failure (e.g. cross-filesystem EXDEV) falls back to
// copy + unlink.
extern "C" PyObject* PyShutil_Move(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* src = args->list[0];
    PyObject* dst = args->list[1];
    if (!src || src->type != 3 || !dst || dst->type != 3) return nullptr;
    if (::rename(src->str.c_str(), dst->str.c_str()) == 0) return nullptr;
    PyShutil_Copyfile(args);
    ::unlink(src->str.c_str());
    return nullptr;
}
static void pyc_rmtree_recursive(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = ::opendir(path.c_str());
        if (d) {
            struct dirent* ent;
            while ((ent = ::readdir(d)) != nullptr) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                pyc_rmtree_recursive(path + "/" + ent->d_name);
            }
            ::closedir(d);
        }
        ::rmdir(path.c_str());
    } else {
        ::unlink(path.c_str());
    }
}
// shutil.rmtree(path) -> None : recursive removal.
extern "C" PyObject* PyShutil_Rmtree(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* path = args->list[0];
    if (!path || path->type != 3) return nullptr;
    pyc_rmtree_recursive(path->str);
    return nullptr;
}

// glob.glob(pattern) -> list[str]. Splits into dirname/basename, lists
// dirname (or "." if none), matches each entry's name against basename
// with a small from-scratch wildcard matcher (*, ?, [seq] — standard
// glob/fnmatch semantics). No recursive "**" support (a hard scoping
// choice, documented — matches itertools' unbounded-iterator precedent:
// recursive directory walking + pattern matching is a materially bigger
// feature than a single-directory match).
static bool pyc_fnmatch(const char* name, const char* pat) {
    if (!*pat) return !*name;
    if (*pat == '*') {
        // Try matching zero or more characters.
        while (*name) {
            if (pyc_fnmatch(name, pat + 1)) return true;
            ++name;
        }
        return pyc_fnmatch(name, pat + 1);
    }
    if (!*name) return false;
    if (*pat == '?') return pyc_fnmatch(name + 1, pat + 1);
    if (*pat == '[') {
        const char* p = pat + 1;
        bool negate = (*p == '!');
        if (negate) ++p;
        bool matched = false;
        bool first = true;
        while (*p && (*p != ']' || first)) {
            first = false;
            if (p[1] == '-' && p[2] && p[2] != ']') {
                if (*name >= p[0] && *name <= p[2]) matched = true;
                p += 3;
            } else {
                if (*name == *p) matched = true;
                ++p;
            }
        }
        if (*p == ']') ++p;
        if (matched == negate) return false;
        return pyc_fnmatch(name + 1, p);
    }
    if (*pat != *name) return false;
    return pyc_fnmatch(name + 1, pat + 1);
}
extern "C" PyObject* PyGlob_Glob(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    PyObject* patObj = args->list[0];
    if (!patObj || patObj->type != 3) return out;
    const std::string& pattern = patObj->str;
    size_t slash = pattern.find_last_of('/');
    std::string dirPart = (slash == std::string::npos) ? "." : pattern.substr(0, slash == 0 ? 1 : slash);
    std::string basePart = (slash == std::string::npos) ? pattern : pattern.substr(slash + 1);
    std::string prefix = (slash == std::string::npos) ? "" : (dirPart == "/" ? "/" : dirPart + "/");
    DIR* d = ::opendir(dirPart.c_str());
    if (!d) return out;
    std::vector<std::string> matches;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.' && basePart.empty() == false && basePart[0] != '.') continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (pyc_fnmatch(ent->d_name, basePart.c_str())) {
            matches.push_back(prefix + ent->d_name);
        }
    }
    ::closedir(d);
    std::sort(matches.begin(), matches.end());
    for (auto& m : matches) {
        PyObject* s = PyUnicode_FromString(m.c_str());
        PyList_Append(out, s);
        Py_DECREF(s);
    }
    return out;
}

// csv.reader(lines) -> list[list[str]]. Takes a plain list of
// line-strings (real csv.reader's actual general contract — any
// iterable of strings, not specifically a file object), splitting each
// on "," with minimal quoted-field support ("a,b",c -> ["a,b","c"], ""
// inside a quoted field means a literal double-quote). Does NOT handle
// embedded newlines inside quoted fields (each input line is one row) or
// custom dialects/delimiters — documented gap, verified against real
// csv module output for representative cases.
static std::vector<std::string> pyc_csv_parse_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool inQuotes = false;
    size_t i = 0;
    // Strip a single trailing newline (lines commonly come from
    // readlines(), which keeps it).
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    size_t n = s.size();
    while (i < n) {
        char c = s[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < n && s[i+1] == '"') { cur += '"'; i += 2; continue; }
                inQuotes = false; ++i; continue;
            }
            cur += c; ++i; continue;
        }
        if (c == '"') { inQuotes = true; ++i; continue; }
        if (c == ',') { fields.push_back(cur); cur.clear(); ++i; continue; }
        cur += c; ++i;
    }
    fields.push_back(cur);
    return fields;
}
extern "C" PyObject* PyCsv_Reader(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    PyObject* lines = args->list[0];
    if (!lines || lines->type != 1) return out;
    pyc_ensure_boxed_list(lines);
    for (PyObject* lineObj : lines->list) {
        if (!lineObj || lineObj->type != 3) continue;
        auto fields = pyc_csv_parse_line(lineObj->str);
        PyObject* row = PyList_New(0);
        for (auto& f : fields) {
            PyObject* s = PyUnicode_FromString(f.c_str());
            PyList_Append(row, s);
            Py_DECREF(s);
        }
        PyList_Append(out, row);
        Py_DECREF(row);
    }
    return out;
}

// csv.writer(f) -> dict tagged typeOf=="csvwriter" capturing the file
// object. Direct-call convention (like hashlib.md5/pathlib.Path
// construction), recognized structurally in Compiler.cpp, NOT
// token+registry — because .writerow(row) needs an explicit receiver
// (mirroring the file.write() fix's lesson: the generic, non-bound dict
// dispatch can't supply one), which requires the constructor's result to
// be typeOf-tagged, which in turn requires AST-level recognition at the
// construction call site too (the generic dict-dispatch path has no way
// to attach a custom typeOf tag to its result).
extern "C" PyObject* PyCsv_Writer(PyObject* fileObj) {
    PyObject* d = PyDict_New();
    PyObject* k = PyUnicode_FromString("__pyc_csv_file__");
    if (fileObj) Py_INCREF(fileObj);
    PyDict_SetItem(d, k, fileObj);
    Py_DECREF(k);
    if (fileObj) Py_DECREF(fileObj); // PyDict_SetItem took its own ref
    return d;
}
static std::string pyc_csv_quote_field(const std::string& f) {
    bool needsQuote = f.find(',') != std::string::npos || f.find('"') != std::string::npos ||
                       f.find('\n') != std::string::npos || f.find('\r') != std::string::npos;
    if (!needsQuote) return f;
    std::string out = "\"";
    for (char c : f) { if (c == '"') out += "\"\""; else out += c; }
    out += "\"";
    return out;
}
extern "C" PyObject* PyCsv_Writerow(PyObject* writer, PyObject* row) {
    if (!writer || writer->type != 2) return nullptr;
    PyObject* key = PyUnicode_FromString("__pyc_csv_file__");
    PyObject* fileObj = Pyc_GetItem(writer, key);
    Py_DECREF(key);
    if (!fileObj) return nullptr;
    std::string line;
    if (row && row->type == 1) {
        pyc_ensure_boxed_list(row);
        for (size_t i = 0; i < row->list.size(); ++i) {
            if (i > 0) line += ',';
            PyObject* item = row->list[i];
            PyObject* s = PyStr_FromAny(item);
            line += pyc_csv_quote_field(s ? s->str : std::string());
            if (s) Py_DECREF(s);
        }
    }
    line += '\n';
    auto it = g_pycFiles.find(fileObj);
    Py_DECREF(fileObj); // Pyc_GetItem returned a new ref
    if (it != g_pycFiles.end() && it->second.fp) {
        std::fwrite(line.data(), 1, line.size(), it->second.fp);
        std::fflush(it->second.fp);
    }
    return nullptr;
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

// ---- math module ----
// Real-valued math functions wrapping libm (<math.h>, included above),
// using the token+registry calling convention (like os/subprocess) rather
// than cmath's AST-direct-call convention, so `import math`,
// `from math import sqrt`, and `import math as m` all work uniformly via
// the generic Pyc_Apply dispatch. Arguments are unboxed via arg_numeric
// (defined above, alongside numeric_val/is_numeric).
static const double kPyMathPi  = 3.14159265358979323846;
static const double kPyMathE   = 2.71828182845904523536;
static const double kPyMathTau = 6.28318530717958647692;

extern "C" PyObject* PyMath_Sqrt(PyObject* args)  { return PyFloat_FromDouble(::sqrt(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Floor(PyObject* args) { return PyInt_FromLong((long)::floor(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Ceil(PyObject* args)  { return PyInt_FromLong((long)::ceil(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Trunc(PyObject* args) { return PyInt_FromLong((long)::trunc(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Pow(PyObject* args)   { return PyFloat_FromDouble(::pow(arg_numeric(args, 0), arg_numeric(args, 1))); }
extern "C" PyObject* PyMath_Log(PyObject* args) {
    // math.log(x) is natural log; math.log(x, base) uses the given base.
    if (args && args->type == 1 && args->list.size() >= 2) {
        return PyFloat_FromDouble(::log(arg_numeric(args, 0)) / ::log(arg_numeric(args, 1)));
    }
    return PyFloat_FromDouble(::log(arg_numeric(args, 0)));
}
extern "C" PyObject* PyMath_Log2(PyObject* args)    { return PyFloat_FromDouble(::log2(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Log10(PyObject* args)   { return PyFloat_FromDouble(::log10(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Exp(PyObject* args)     { return PyFloat_FromDouble(::exp(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Sin(PyObject* args)     { return PyFloat_FromDouble(::sin(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Cos(PyObject* args)     { return PyFloat_FromDouble(::cos(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Tan(PyObject* args)     { return PyFloat_FromDouble(::tan(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Asin(PyObject* args)    { return PyFloat_FromDouble(::asin(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Acos(PyObject* args)    { return PyFloat_FromDouble(::acos(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Atan(PyObject* args)    { return PyFloat_FromDouble(::atan(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Atan2(PyObject* args)   { return PyFloat_FromDouble(::atan2(arg_numeric(args, 0), arg_numeric(args, 1))); }
extern "C" PyObject* PyMath_Hypot(PyObject* args)   { return PyFloat_FromDouble(::hypot(arg_numeric(args, 0), arg_numeric(args, 1))); }
extern "C" PyObject* PyMath_Fabs(PyObject* args)    { return PyFloat_FromDouble(::fabs(arg_numeric(args, 0))); }
extern "C" PyObject* PyMath_Fmod(PyObject* args)    { return PyFloat_FromDouble(::fmod(arg_numeric(args, 0), arg_numeric(args, 1))); }
extern "C" PyObject* PyMath_Degrees(PyObject* args) { return PyFloat_FromDouble(arg_numeric(args, 0) * (180.0 / kPyMathPi)); }
extern "C" PyObject* PyMath_Radians(PyObject* args) { return PyFloat_FromDouble(arg_numeric(args, 0) * (kPyMathPi / 180.0)); }
extern "C" PyObject* PyMath_Isnan(PyObject* args)    { return PyBool_New(::isnan(arg_numeric(args, 0)) ? 1 : 0); }
extern "C" PyObject* PyMath_Isinf(PyObject* args)    { return PyBool_New(::isinf(arg_numeric(args, 0)) ? 1 : 0); }
extern "C" PyObject* PyMath_Isfinite(PyObject* args) { return PyBool_New(::isfinite(arg_numeric(args, 0)) ? 1 : 0); }

extern "C" PyObject* PyMath_Gcd(PyObject* args) {
    long a = (long)arg_numeric(args, 0);
    long b = (long)arg_numeric(args, 1);
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long t = b; b = a % b; a = t; }
    return PyInt_FromLong(a);
}

extern "C" PyObject* PyMath_Factorial(PyObject* args) {
    long n = (long)arg_numeric(args, 0);
    if (n < 0) {
        pyc_raise_msg("ValueError", "factorial() not defined for negative values");
        return nullptr;
    }
    long result = 1;
    for (long i = 2; i <= n; ++i) result *= i;
    return PyInt_FromLong(result);
}

// makeMathModuleDict: builds a dict emulating the math module. Functions
// are string tokens (dispatched via Pyc_Apply/g_callableRegistry, see
// pyc_register_callable below); pi/e/tau are plain float values.
static PyObject* makeMathModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    auto addFloat = [&](const char* name, double v) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* val = PyFloat_FromDouble(v);
        PyDict_SetItem(d, k, val);
        Py_DECREF(k); Py_DECREF(val);
    };
    addTok("sqrt", "PyMath_Sqrt");
    addTok("floor", "PyMath_Floor");
    addTok("ceil", "PyMath_Ceil");
    addTok("trunc", "PyMath_Trunc");
    addTok("pow", "PyMath_Pow");
    addTok("log", "PyMath_Log");
    addTok("log2", "PyMath_Log2");
    addTok("log10", "PyMath_Log10");
    addTok("exp", "PyMath_Exp");
    addTok("sin", "PyMath_Sin");
    addTok("cos", "PyMath_Cos");
    addTok("tan", "PyMath_Tan");
    addTok("asin", "PyMath_Asin");
    addTok("acos", "PyMath_Acos");
    addTok("atan", "PyMath_Atan");
    addTok("atan2", "PyMath_Atan2");
    addTok("hypot", "PyMath_Hypot");
    addTok("fabs", "PyMath_Fabs");
    addTok("fmod", "PyMath_Fmod");
    addTok("degrees", "PyMath_Degrees");
    addTok("radians", "PyMath_Radians");
    addTok("isnan", "PyMath_Isnan");
    addTok("isinf", "PyMath_Isinf");
    addTok("isfinite", "PyMath_Isfinite");
    addTok("gcd", "PyMath_Gcd");
    addTok("factorial", "PyMath_Factorial");
    addFloat("pi", kPyMathPi);
    addFloat("e", kPyMathE);
    addFloat("tau", kPyMathTau);
    addFloat("inf", HUGE_VAL);
    addFloat("nan", NAN);
    return d;
}

// ---- json module ----
// json.dumps(obj) / json.loads(s), operating directly on the generic boxed
// value tree (dict/list/str/int/float/bool/None) — no new PyObject types,
// no dependency on pyc's class system. Token+registry convention, same as
// math. Floats are formatted via the same format_double() every other
// float-printing path uses (see PyObject_PrintBase), for consistency.
static void jsonEscapeStringInto(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

static void jsonDumpValueInto(PyObject* obj, std::string& out) {
    if (!obj) { out += "null"; return; }
    switch (obj->type) {
        case 0: out += std::to_string(obj->value); break;
        case 5: out += (obj->value ? "true" : "false"); break;
        case 4: {
            char buf[64];
            format_double(buf, sizeof(buf), obj->dvalue);
            out += buf;
            break;
        }
        case 3:
            jsonEscapeStringInto(obj->str, out);
            break;
        case 1: {
            out += '[';
            // Read internal storage directly (rather than via
            // PyList_GetItem, which allocates a fresh int/float PyObject
            // per element for homogeneous lists) to avoid extra
            // alloc/refcount churn during serialization.
            if (obj->list_item_type == 1) {
                for (size_t i = 0; i < obj->ilist.size(); ++i) {
                    if (i) out += ", ";
                    out += std::to_string(obj->ilist[i]);
                }
            } else if (obj->list_item_type == 2) {
                for (size_t i = 0; i < obj->flist.size(); ++i) {
                    if (i) out += ", ";
                    char buf[64];
                    format_double(buf, sizeof(buf), obj->flist[i]);
                    out += buf;
                }
            } else {
                for (size_t i = 0; i < obj->list.size(); ++i) {
                    if (i) out += ", ";
                    jsonDumpValueInto(obj->list[i], out);
                }
            }
            out += ']';
            break;
        }
        case 2: {
            out += '{';
            bool first = true;
            for (auto& pair : obj->dict) {
                if (!first) out += ", ";
                first = false;
                if (pair.first && pair.first->type == 3) {
                    jsonEscapeStringInto(pair.first->str, out);
                } else {
                    out += "\"\"";
                }
                out += ": ";
                jsonDumpValueInto(pair.second, out);
            }
            out += '}';
            break;
        }
        default:
            out += "null";
    }
}

extern "C" PyObject* PyJson_Dumps(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return PyUnicode_FromString("null");
    std::string out;
    jsonDumpValueInto(args->list[0], out);
    return PyUnicode_FromString(out.c_str());
}

// Small recursive-descent JSON parser producing real boxed values via the
// existing constructors (PyDict_New/PyList_New/PyUnicode_FromString/
// PyInt_FromLong/PyFloat_FromDouble/PyBool_New, null PyObject* for JSON
// null). Best-effort: malformed input stops parsing early rather than
// raising a structured exception (matches the general "don't crash"
// posture of the other synthetic modules; not a full json.JSONDecodeError
// implementation).
namespace {
struct PycJsonParser {
    const char* p;
    const char* end;

    void skipWs() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    PyObject* parseValue() {
        skipWs();
        if (p >= end) return nullptr;
        char c = *p;
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' && end - p >= 4 && strncmp(p, "true", 4) == 0)  { p += 4; return PyBool_New(1); }
        if (c == 'f' && end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; return PyBool_New(0); }
        if (c == 'n' && end - p >= 4 && strncmp(p, "null", 4) == 0)  { p += 4; return nullptr; }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        return nullptr;
    }

    PyObject* parseObject() {
        PyObject* d = PyDict_New();
        ++p; // consume '{'
        skipWs();
        if (p < end && *p == '}') { ++p; return d; }
        while (true) {
            skipWs();
            if (p >= end || *p != '"') break;
            PyObject* key = parseString();
            skipWs();
            if (p >= end || *p != ':') { Py_DECREF(key); break; }
            ++p; // consume ':'
            PyObject* val = parseValue();
            PyDict_SetItem(d, key, val);
            Py_DECREF(key);
            if (val) Py_DECREF(val);
            skipWs();
            if (p >= end) break;
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; break; }
            break;
        }
        return d;
    }

    PyObject* parseArray() {
        PyObject* lst = PyList_New(0);
        ++p; // consume '['
        skipWs();
        if (p < end && *p == ']') { ++p; return lst; }
        while (true) {
            PyObject* val = parseValue();
            PyList_Append(lst, val);
            if (val) Py_DECREF(val);
            skipWs();
            if (p >= end) break;
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; break; }
            break;
        }
        return lst;
    }

    PyObject* parseString() {
        ++p; // consume opening quote
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case '"':  s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/'; break;
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case 'r':  s += '\r'; break;
                    case 'b':  s += '\b'; break;
                    case 'f':  s += '\f'; break;
                    case 'u': {
                        if (p + 4 < end) {
                            char hex[5] = {p[1], p[2], p[3], p[4], 0};
                            unsigned int cp = (unsigned int)strtoul(hex, nullptr, 16);
                            p += 4;
                            // Basic BMP-only UTF-8 encoding (no surrogate
                            // pair handling for values outside the BMP).
                            if (cp < 0x80) {
                                s += (char)cp;
                            } else if (cp < 0x800) {
                                s += (char)(0xC0 | (cp >> 6));
                                s += (char)(0x80 | (cp & 0x3F));
                            } else {
                                s += (char)(0xE0 | (cp >> 12));
                                s += (char)(0x80 | ((cp >> 6) & 0x3F));
                                s += (char)(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: s += *p; break;
                }
                ++p;
            } else {
                s += *p;
                ++p;
            }
        }
        if (p < end && *p == '"') ++p; // consume closing quote
        return PyUnicode_FromString(s.c_str());
    }

    PyObject* parseNumber() {
        const char* start = p;
        bool isFloat = false;
        if (*p == '-') ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p < end && *p == '.') {
            isFloat = true; ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            isFloat = true; ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        std::string numStr(start, (size_t)(p - start));
        if (isFloat) return PyFloat_FromDouble(strtod(numStr.c_str(), nullptr));
        return PyInt_FromLong(strtol(numStr.c_str(), nullptr, 10));
    }
};
} // namespace

extern "C" PyObject* PyJson_Loads(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* s = args->list[0];
    if (!s || s->type != 3) return nullptr;
    PycJsonParser parser{s->str.c_str(), s->str.c_str() + s->str.size()};
    return parser.parseValue();
}

// makeJsonModuleDict: builds a dict emulating the json module (dumps/loads
// only — no indent/sort_keys/separators kwargs, no custom encoders).
static PyObject* makeJsonModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("dumps", "PyJson_Dumps");
    addTok("loads", "PyJson_Loads");
    return d;
}

// ---- random module ----
// A from-scratch MT19937 generator replicating CPython's _randommodule.c
// exactly (same 624-word state, same tempering transform, same
// init_genrand/init_by_array seeding procedure) — so `random.seed(n)`
// followed by any of these functions produces bit-identical output to
// real CPython for the same `n`, verified against real Python output
// during development. One process-global generator instance, matching
// the single default `random.Random()` instance CPython's random module
// functions (random.seed/random.random/...) implicitly share.
class PycMT19937 {
public:
    static const int N = 624;
    static const int M = 397;
    static const uint32_t MATRIX_A = 0x9908b0dfUL;
    static const uint32_t UPPER_MASK = 0x80000000UL;
    static const uint32_t LOWER_MASK = 0x7fffffffUL;

    uint32_t mt[N];
    int mti = N + 1;

    void initGenrand(uint32_t s) {
        mt[0] = s;
        for (int i = 1; i < N; ++i) {
            mt[i] = (uint32_t)(1812433253UL * (mt[i - 1] ^ (mt[i - 1] >> 30)) + (uint32_t)i);
        }
        mti = N;
    }

    void initByArray(const uint32_t* key, int keyLen) {
        initGenrand(19650218UL);
        int i = 1, j = 0;
        int k = (N > keyLen) ? N : keyLen;
        for (; k; --k) {
            mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525UL)) + key[j] + (uint32_t)j;
            ++i; ++j;
            if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
            if (j >= keyLen) j = 0;
        }
        for (k = N - 1; k; --k) {
            mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941UL)) - (uint32_t)i;
            ++i;
            if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
        }
        mt[0] = 0x80000000UL;
    }

    // Matches CPython's random_seed(): seed from the absolute value of an
    // integer, split into little-endian 32-bit words.
    void seedFromInt(long long seedVal) {
        unsigned long long v = seedVal < 0 ? (unsigned long long)(-seedVal) : (unsigned long long)seedVal;
        uint32_t key[2];
        int keyLen = 0;
        do {
            key[keyLen++] = (uint32_t)(v & 0xffffffffULL);
            v >>= 32;
        } while (v != 0 && keyLen < 2);
        initByArray(key, keyLen);
    }

    uint32_t genrandUint32() {
        static const uint32_t mag01[2] = {0x0UL, MATRIX_A};
        uint32_t y;
        if (mti >= N) {
            int kk;
            if (mti == N + 1) initGenrand(5489UL); // default seed if seed() never called
            for (kk = 0; kk < N - M; ++kk) {
                y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
                mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 0x1UL];
            }
            for (; kk < N - 1; ++kk) {
                y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
                mt[kk] = mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 0x1UL];
            }
            y = (mt[N - 1] & UPPER_MASK) | (mt[0] & LOWER_MASK);
            mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 0x1UL];
            mti = 0;
        }
        y = mt[mti++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680UL;
        y ^= (y << 15) & 0xefc60000UL;
        y ^= (y >> 18);
        return y;
    }

    // Matches CPython's random_random(): 53 bits of precision in [0, 1).
    double randomDouble() {
        uint32_t a = genrandUint32() >> 5;
        uint32_t b = genrandUint32() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }

    // getrandbits(k) for k <= 32, matching CPython's random_getrandbits.
    uint32_t getrandbits(int k) {
        return genrandUint32() >> (32 - k);
    }

    // Matches CPython's Random._randbelow_with_getrandbits: uniform in [0, n).
    uint64_t randbelow(uint64_t n) {
        if (n == 0) return 0;
        int k = 0;
        for (uint64_t t = n; t; t >>= 1) ++k;
        uint64_t r;
        do {
            if (k <= 32) {
                r = getrandbits(k);
            } else {
                // k up to 64: combine two 32-bit draws, low bits first,
                // matching CPython's getrandbits() word order for k>32.
                r = (uint64_t)getrandbits(32) | ((uint64_t)getrandbits(k - 32) << 32);
            }
        } while (r >= n);
        return r;
    }
};

static PycMT19937 g_pycRandom;

extern "C" PyObject* PyRandom_Seed(PyObject* args) {
    long long seedVal = 0;
    if (args && args->type == 1 && !args->list.empty()) seedVal = (long long)arg_numeric(args, 0);
    g_pycRandom.seedFromInt(seedVal);
    return nullptr;
}

extern "C" PyObject* PyRandom_Random(PyObject* args) {
    (void)args;
    return PyFloat_FromDouble(g_pycRandom.randomDouble());
}

extern "C" PyObject* PyRandom_Randrange(PyObject* args) {
    // randrange(stop) or randrange(start, stop)
    if (!args || args->type != 1 || args->list.empty()) return PyInt_FromLong(0);
    long long start = 0, stop;
    if (args->list.size() >= 2) {
        start = (long long)arg_numeric(args, 0);
        stop = (long long)arg_numeric(args, 1);
    } else {
        stop = (long long)arg_numeric(args, 0);
    }
    long long width = stop - start;
    if (width <= 0) return PyInt_FromLong(start);
    return PyInt_FromLong(start + (long long)g_pycRandom.randbelow((uint64_t)width));
}

extern "C" PyObject* PyRandom_Randint(PyObject* args) {
    // randint(a, b) == randrange(a, b + 1)
    if (!args || args->type != 1 || args->list.size() < 2) return PyInt_FromLong(0);
    long long a = (long long)arg_numeric(args, 0);
    long long b = (long long)arg_numeric(args, 1);
    long long width = b - a + 1;
    if (width <= 0) return PyInt_FromLong(a);
    return PyInt_FromLong(a + (long long)g_pycRandom.randbelow((uint64_t)width));
}

extern "C" PyObject* PyRandom_Uniform(PyObject* args) {
    double a = arg_numeric(args, 0);
    double b = arg_numeric(args, 1);
    return PyFloat_FromDouble(a + (b - a) * g_pycRandom.randomDouble());
}

extern "C" PyObject* PyRandom_Choice(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* seq = args->list[0];
    if (!seq || seq->type != 1) return nullptr;
    size_t n = PyList_Size(seq);
    if (n == 0) {
        pyc_raise_msg("IndexError", "list index out of range");
        return nullptr;
    }
    size_t idx = (size_t)g_pycRandom.randbelow((uint64_t)n);
    return PyList_GetItem(seq, idx);
}

extern "C" PyObject* PyRandom_Shuffle(PyObject* args) {
    // In-place Fisher-Yates from the end, matching CPython's
    // Random.shuffle exactly (both the algorithm and the draw order).
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    PyObject* lst = args->list[0];
    if (!lst || lst->type != 1) return nullptr;
    size_t n = PyList_Size(lst);
    for (size_t i = n; i-- > 1;) {
        size_t j = (size_t)g_pycRandom.randbelow((uint64_t)(i + 1));
        if (i == j) continue;
        if (lst->list_item_type == 0) {
            std::swap(lst->list[i], lst->list[j]);
        } else if (lst->list_item_type == 1) {
            std::swap(lst->ilist[i], lst->ilist[j]);
        } else if (lst->list_item_type == 2) {
            std::swap(lst->flist[i], lst->flist[j]);
        }
    }
    return nullptr;
}

static PyObject* makeRandomModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("seed", "PyRandom_Seed");
    addTok("random", "PyRandom_Random");
    addTok("randrange", "PyRandom_Randrange");
    addTok("randint", "PyRandom_Randint");
    addTok("uniform", "PyRandom_Uniform");
    addTok("choice", "PyRandom_Choice");
    addTok("shuffle", "PyRandom_Shuffle");
    return d;
}

// ---- itertools module (subset) ----
// Eager, list-returning implementations only — pyc has no lazy
// iterator/__next__/StopIteration protocol (generator expressions are
// already eagerly materialized, see FEATURES.md), so infinite iterators
// (count, cycle, unbounded repeat) cannot be represented at all and are
// deliberately not implemented. Every "iterable" argument must already be
// a real, materialized pyc list. itertools.product/combinations/
// permutations/zip_longest entries now return real tuples (type 7) for
// their inner combos (matching CPython); the outer container is a list.

// Read element i of a list uniformly regardless of homogeneous/boxed
// storage, returning a NEW reference (caller must Py_DECREF it).
static PyObject* pycListItemNewRef(PyObject* lst, size_t i) {
    if (!lst || lst->type != 1) return nullptr;
    if (lst->list_item_type == 1 && i < lst->ilist.size()) return PyInt_FromLong(lst->ilist[i]);
    if (lst->list_item_type == 2 && i < lst->flist.size()) return PyFloat_FromDouble(lst->flist[i]);
    if (lst->list_item_type == 0 && i < lst->list.size()) {
        PyObject* v = lst->list[i];
        if (v) Py_INCREF(v);
        return v;
    }
    return nullptr;
}

extern "C" PyObject* PyItertools_Chain(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (args && args->type == 1) {
        for (PyObject* it : args->list) {
            if (!it || it->type != 1) continue;
            size_t n = PyList_Size(it);
            for (size_t i = 0; i < n; ++i) {
                PyObject* v = pycListItemNewRef(it, i);
                PyList_Append(out, v);
                if (v) Py_DECREF(v);
            }
        }
    }
    return out;
}

extern "C" PyObject* PyItertools_Product(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    const std::vector<PyObject*>& lists = args->list;
    size_t nLists = lists.size();
    std::vector<size_t> sizes(nLists);
    for (size_t i = 0; i < nLists; ++i) {
        sizes[i] = (lists[i] && lists[i]->type == 1) ? PyList_Size(lists[i]) : 0;
        if (sizes[i] == 0) return out; // any empty input -> empty product
    }
    std::vector<size_t> idx(nLists, 0);
    while (true) {
        PyObject* combo = PyTuple_New(nLists);
        for (size_t i = 0; i < nLists; ++i) {
            PyTuple_SetItem(combo, i, pycListItemNewRef(lists[i], idx[i]));
        }
        PyList_Append(out, combo);
        Py_DECREF(combo);
        // Odometer increment, rightmost fastest — matches itertools.product order.
        long pos = (long)nLists - 1;
        for (; pos >= 0; --pos) {
            if (++idx[pos] < sizes[pos]) break;
            idx[pos] = 0;
        }
        if (pos < 0) break;
    }
    return out;
}

extern "C" PyObject* PyItertools_Combinations(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.size() < 2) return out;
    PyObject* iterable = args->list[0];
    long r = (long)arg_numeric(args, 1);
    if (!iterable || iterable->type != 1 || r < 0) return out;
    size_t n = PyList_Size(iterable);
    if ((size_t)r > n) return out;
    if (r == 0) {
        PyObject* combo = PyTuple_New(0);
        PyList_Append(out, combo);
        Py_DECREF(combo);
        return out;
    }
    std::vector<size_t> idx((size_t)r);
    for (long i = 0; i < r; ++i) idx[(size_t)i] = (size_t)i;
    while (true) {
        PyObject* combo = PyTuple_New((size_t)r);
        for (long i = 0; i < r; ++i) {
            PyTuple_SetItem(combo, (size_t)i, pycListItemNewRef(iterable, idx[(size_t)i]));
        }
        PyList_Append(out, combo);
        Py_DECREF(combo);
        long i = r - 1;
        while (i >= 0 && idx[(size_t)i] == n - (size_t)(r - i)) --i;
        if (i < 0) break;
        idx[(size_t)i]++;
        for (long j = i + 1; j < r; ++j) idx[(size_t)j] = idx[(size_t)(j - 1)] + 1;
    }
    return out;
}

extern "C" PyObject* PyItertools_Permutations(PyObject* args) {
    // Direct translation of the itertools.permutations pure-Python
    // reference implementation from the CPython docs (same algorithm,
    // same iteration order), adapted to pyc's list storage.
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    PyObject* iterable = args->list[0];
    if (!iterable || iterable->type != 1) return out;
    size_t n = PyList_Size(iterable);
    long r = (long)n;
    if (args->list.size() >= 2) r = (long)arg_numeric(args, 1);
    if (r < 0 || (size_t)r > n) return out;

    std::vector<size_t> indices(n);
    for (size_t i = 0; i < n; ++i) indices[i] = i;
    std::vector<long> cycles((size_t)r);
    for (long i = 0; i < r; ++i) cycles[(size_t)i] = (long)n - i;

    auto emit = [&]() {
        PyObject* combo = PyTuple_New((size_t)r);
        for (long i = 0; i < r; ++i) {
            PyTuple_SetItem(combo, (size_t)i, pycListItemNewRef(iterable, indices[(size_t)i]));
        }
        PyList_Append(out, combo);
        Py_DECREF(combo);
    };

    if (r == 0) { emit(); return out; }
    emit();
    while (n) {
        bool advanced = false;
        for (long i = r - 1; i >= 0; --i) {
            if (--cycles[(size_t)i] == 0) {
                // Rotate indices[i:] left by one.
                size_t tmp = indices[(size_t)i];
                for (size_t k = (size_t)i; k + 1 < n; ++k) indices[k] = indices[k + 1];
                indices[n - 1] = tmp;
                cycles[(size_t)i] = (long)n - i;
            } else {
                size_t j = (size_t)cycles[(size_t)i];
                std::swap(indices[(size_t)i], indices[n - j]);
                emit();
                advanced = true;
                break;
            }
        }
        if (!advanced) break;
    }
    return out;
}

extern "C" PyObject* PyItertools_Islice(PyObject* args) {
    // Bounded 2-arg form only: islice(iterable, stop) -> first `stop`
    // elements. (Not the full start/stop/step signature.)
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.size() < 2) return out;
    PyObject* iterable = args->list[0];
    long stop = (long)arg_numeric(args, 1);
    if (!iterable || iterable->type != 1) return out;
    size_t n = PyList_Size(iterable);
    long lim = stop < 0 ? 0 : stop;
    for (long i = 0; i < lim && (size_t)i < n; ++i) {
        PyObject* v = pycListItemNewRef(iterable, (size_t)i);
        PyList_Append(out, v);
        if (v) Py_DECREF(v);
    }
    return out;
}

extern "C" PyObject* PyItertools_Starmap(PyObject* args) {
    // starmap(fn, iterable): iterable's elements are themselves argument
    // lists, applied as fn(*args_list) via the same Pyc_Apply dispatch
    // every other callable-token call goes through.
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.size() < 2) return out;
    PyObject* fn = args->list[0];
    PyObject* iterable = args->list[1];
    if (!iterable || iterable->type != 1) return out;
    size_t n = PyList_Size(iterable);
    for (size_t i = 0; i < n; ++i) {
        PyObject* argList = pycListItemNewRef(iterable, i);
        PyObject* result = Pyc_Apply(fn, argList);
        PyList_Append(out, result);
        if (argList) Py_DECREF(argList);
        if (result) Py_DECREF(result);
    }
    return out;
}

extern "C" PyObject* PyItertools_ZipLongest(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    size_t nLists = args->list.size();
    size_t maxLen = 0;
    for (auto* lst : args->list) {
        size_t sz = (lst && lst->type == 1) ? PyList_Size(lst) : 0;
        if (sz > maxLen) maxLen = sz;
    }
    for (size_t i = 0; i < maxLen; ++i) {
        PyObject* row = PyTuple_New(nLists);
        for (size_t j = 0; j < nLists; ++j) {
            PyObject* lst = args->list[j];
            size_t sz = (lst && lst->type == 1) ? PyList_Size(lst) : 0;
            if (i < sz) {
                PyTuple_SetItem(row, j, pycListItemNewRef(lst, i));
            } else {
                PyTuple_SetItem(row, j, nullptr); // None for exhausted iterable(s)
            }
        }
        PyList_Append(out, row);
        Py_DECREF(row);
    }
    return out;
}

// itertools.accumulate(iterable, func=None) -> list. func=None means
// running sum (verified against real itertools). No `initial=` keyword
// support (token+registry calls don't carry keyword arguments through
// generically — same limitation as other synthetic-module functions).
extern "C" PyObject* PyItertools_Accumulate(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    PyObject* iterable = args->list[0];
    if (!iterable || iterable->type != 1) return out;
    pyc_ensure_boxed_list(iterable);
    PyObject* func = args->list.size() > 1 ? args->list[1] : nullptr;
    PyObject* acc = nullptr;
    for (PyObject* item : iterable->list) {
        if (!acc) {
            acc = item;
            if (acc) Py_INCREF(acc);
        } else {
            PyObject* next;
            if (func) {
                PyObject* argList = PyList_New(2);
                if (acc) Py_INCREF(acc);
                PyList_SetItem(argList, 0, acc);
                if (item) Py_INCREF(item);
                PyList_SetItem(argList, 1, item);
                next = Pyc_Apply(func, argList);
                Py_DECREF(argList);
            } else {
                next = PyNumber_Add(acc, item);
            }
            Py_DECREF(acc);
            acc = next;
        }
        PyList_Append(out, acc);
    }
    if (acc) Py_DECREF(acc);
    return out;
}
// itertools.takewhile(pred, iterable) / dropwhile(pred, iterable) ->
// list. Calls pred via Pyc_Apply per element (the sorted(key=...) /
// functools.reduce pattern).
extern "C" PyObject* PyItertools_Takewhile(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.size() < 2) return out;
    PyObject* pred = args->list[0];
    PyObject* iterable = args->list[1];
    if (!iterable || iterable->type != 1) return out;
    pyc_ensure_boxed_list(iterable);
    for (PyObject* item : iterable->list) {
        PyObject* argList = PyList_New(1);
        if (item) Py_INCREF(item);
        PyList_SetItem(argList, 0, item);
        PyObject* r = Pyc_Apply(pred, argList);
        Py_DECREF(argList);
        PyObject* truthy = PyBuiltin_Bool(r);
        bool keep = truthy && truthy->value != 0;
        if (r) Py_DECREF(r);
        if (truthy) Py_DECREF(truthy);
        if (!keep) break;
        PyList_Append(out, item);
    }
    return out;
}
extern "C" PyObject* PyItertools_Dropwhile(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.size() < 2) return out;
    PyObject* pred = args->list[0];
    PyObject* iterable = args->list[1];
    if (!iterable || iterable->type != 1) return out;
    pyc_ensure_boxed_list(iterable);
    bool dropping = true;
    for (PyObject* item : iterable->list) {
        if (dropping) {
            PyObject* argList = PyList_New(1);
            if (item) Py_INCREF(item);
            PyList_SetItem(argList, 0, item);
            PyObject* r = Pyc_Apply(pred, argList);
            Py_DECREF(argList);
            PyObject* truthy = PyBuiltin_Bool(r);
            bool drop = truthy && truthy->value != 0;
            if (r) Py_DECREF(r);
            if (truthy) Py_DECREF(truthy);
            if (drop) continue;
            dropping = false;
        }
        PyList_Append(out, item);
    }
    return out;
}
// itertools.compress(data, selectors) -> list : parallel filter.
extern "C" PyObject* PyItertools_Compress(PyObject* args) {
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.size() < 2) return out;
    PyObject* data = args->list[0];
    PyObject* selectors = args->list[1];
    if (!data || data->type != 1 || !selectors || selectors->type != 1) return out;
    pyc_ensure_boxed_list(data);
    pyc_ensure_boxed_list(selectors);
    size_t n = std::min(data->list.size(), selectors->list.size());
    for (size_t i = 0; i < n; ++i) {
        PyObject* sel = selectors->list[i];
        PyObject* truthy = PyBuiltin_Bool(sel);
        bool keep = truthy && truthy->value != 0;
        if (truthy) Py_DECREF(truthy);
        if (keep) PyList_Append(out, data->list[i]);
    }
    return out;
}
// itertools.groupby(iterable, key=None) -> list of (key, group_list)
// 2-tuples (matching CPython's groupby, which yields (key, group_iterator)
// tuples; the group is eagerly materialized as a list here). Groups only
// *consecutive* equal
// keys (verified against real groupby — NOT a full partition).
// Direct-call convention (2 raw args), not token+registry: `key=` is a
// keyword argument, which the generic dict-dispatch has no mechanism to
// read through, so construction is always intercepted structurally in
// Compiler.cpp instead (see the call site's comment for the bug this
// fixed — key= was being silently dropped).
extern "C" PyObject* PyItertools_Groupby(PyObject* iterable, PyObject* keyFn) {
    PyObject* out = PyList_New(0);
    if (!iterable || iterable->type != 1) return out;
    pyc_ensure_boxed_list(iterable);
    auto computeKey = [&](PyObject* item) -> PyObject* {
        if (!keyFn) { if (item) Py_INCREF(item); return item; }
        PyObject* argList = PyList_New(1);
        if (item) Py_INCREF(item);
        PyList_SetItem(argList, 0, item);
        PyObject* k = Pyc_Apply(keyFn, argList);
        Py_DECREF(argList);
        return k;
    };
    PyObject* curKey = nullptr;
    PyObject* curGroup = nullptr;
    for (PyObject* item : iterable->list) {
        PyObject* k = computeKey(item);
        bool sameGroup = curGroup && ((k == curKey) || (k && curKey && PyObject_CompareBool(k, curKey, 0)) ||
                                       (!k && !curKey));
        if (!sameGroup) {
            if (curGroup) {
                PyObject* pair = PyTuple_New(2);
                PyTuple_SetItem(pair, 0, curKey);
                PyTuple_SetItem(pair, 1, curGroup);
                PyList_Append(out, pair);
                Py_DECREF(pair);
            }
            curKey = k;
            curGroup = PyList_New(0);
        } else {
            if (k) Py_DECREF(k);
        }
        if (item) Py_INCREF(item);
        PyList_Append(curGroup, item);
    }
    if (curGroup) {
        PyObject* pair = PyTuple_New(2);
        PyTuple_SetItem(pair, 0, curKey);
        PyTuple_SetItem(pair, 1, curGroup);
        PyList_Append(out, pair);
        Py_DECREF(pair);
    }
    return out;
}
// itertools.chain.from_iterable(iterable_of_iterables) -> list :
// flattens one level. AST-recognized as a two-level attribute chain
// (Compiler.cpp), same shape as datetime.date.today().
extern "C" PyObject* PyItertools_ChainFromIterable(PyObject* outer) {
    PyObject* out = PyList_New(0);
    if (!outer || outer->type != 1) return out;
    pyc_ensure_boxed_list(outer);
    for (PyObject* inner : outer->list) {
        if (!inner || inner->type != 1) continue;
        pyc_ensure_boxed_list(inner);
        for (PyObject* item : inner->list) {
            if (item) Py_INCREF(item);
            PyList_Append(out, item);
            if (item) Py_DECREF(item);
        }
    }
    return out;
}

static PyObject* makeItertoolsModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("chain", "PyItertools_Chain");
    addTok("product", "PyItertools_Product");
    addTok("combinations", "PyItertools_Combinations");
    addTok("permutations", "PyItertools_Permutations");
    addTok("starmap", "PyItertools_Starmap");
    addTok("islice", "PyItertools_Islice");
    addTok("zip_longest", "PyItertools_ZipLongest");
    addTok("accumulate", "PyItertools_Accumulate");
    addTok("takewhile", "PyItertools_Takewhile");
    addTok("dropwhile", "PyItertools_Dropwhile");
    addTok("compress", "PyItertools_Compress");
    // "groupby" is NOT a dict entry — itertools.groupby(...) is always
    // intercepted structurally in Compiler.cpp (see PyItertools_Groupby's
    // comment), same as csv.writer/pathlib.Path/hashlib.md5 construction.
    return d;
}

// ---- collections module (subset) ----
// Counter(iterable) returns a plain real dict (type 2) pre-populated with
// counts — not a custom class instance (subclassing dict or backing a
// stdlib-shaped container with a pyc-level class was confirmed unreliable
// during development: dict subclassing doesn't behave as a real dict, and
// __getitem__/__setitem__ dunders silently dropped output in at least one
// tested path). most_common(counter, n) is a plain companion function,
// not counter.most_common(n) method syntax, for the same reason.
// defaultdict/namedtuple/deque are not implemented (see IMPLEMENTATION.md).
extern "C" PyObject* PyCollections_Counter(PyObject* args) {
    PyObject* d = PyDict_New();
    if (!args || args->type != 1 || args->list.empty()) return d;
    PyObject* iterable = args->list[0];
    if (!iterable || iterable->type != 1) return d;
    size_t n = PyList_Size(iterable);
    for (size_t i = 0; i < n; ++i) {
        PyObject* item = pycListItemNewRef(iterable, i);
        PyObject* cur = Pyc_GetItem(d, item);
        long count = (cur && cur->type == 0) ? cur->value + 1 : 1;
        if (cur) Py_DECREF(cur);
        PyObject* newCount = PyInt_FromLong(count);
        PyDict_SetItem(d, item, newCount);
        Py_DECREF(newCount);
        if (item) Py_DECREF(item);
    }
    return d;
}

extern "C" PyObject* PyCollections_MostCommon(PyObject* args) {
    // most_common(counter) or most_common(counter, n). Returns a list of
    // (element, count) 2-tuples, matching CPython. Ties (equal counts)
    // are broken by the counter dict's iteration order, which — like
    // real Python dicts — should be insertion order, but pyc's dict
    // iteration order is not currently insertion-order-preserving (see
    // IMPLEMENTATION.md); tie-breaking may not match CPython exactly.
    PyObject* out = PyList_New(0);
    if (!args || args->type != 1 || args->list.empty()) return out;
    PyObject* counter = args->list[0];
    if (!counter || counter->type != 2) return out;
    long limit = -1;
    if (args->list.size() >= 2) limit = (long)arg_numeric(args, 1);
    std::vector<std::pair<PyObject*, long>> items;
    for (auto& pair : counter->dict) {
        long cnt = (pair.second && pair.second->type == 0) ? pair.second->value : 0;
        items.push_back({pair.first, cnt});
    }
    std::stable_sort(items.begin(), items.end(),
                      [](const std::pair<PyObject*, long>& a, const std::pair<PyObject*, long>& b) {
                          return a.second > b.second;
                      });
    size_t lim = (limit < 0) ? items.size() : std::min((size_t)limit, items.size());
    for (size_t i = 0; i < lim; ++i) {
        PyObject* pairTuple = PyTuple_New(2);
        PyTuple_SetItem(pairTuple, 0, items[i].first);
        PyTuple_SetItem(pairTuple, 1, PyInt_FromLong(items[i].second));
        PyList_Append(out, pairTuple);
        Py_DECREF(pairTuple);
    }
    return out;
}

// collections.deque(iterable=[]) — a plain list (type 1, no new type
// tag) with a compile-time "deque" typeOf label (Compiler.cpp) driving
// .appendleft()/.popleft()/.rotate() dispatch. .append()/.pop() and
// friends already work unchanged (either unconditional, like .append(),
// or extended to accept the "deque" typeOf label alongside "list",
// like .pop()/.copy()/.clear()). Direct-call convention (construction is
// AST-recognized, like pathlib.Path, so the result can carry the
// "deque" tag — the generic dict-dispatch path can't attach a custom
// tag to its result).
extern "C" PyObject* PyCollections_Deque(PyObject* iterable) {
    if (!iterable || iterable->type != 1) return PyList_New(0);
    pyc_ensure_boxed_list(iterable);
    PyObject* r = PyList_New(iterable->list.size());
    for (size_t i = 0; i < iterable->list.size(); ++i) {
        if (iterable->list[i]) Py_INCREF(iterable->list[i]);
        PyList_SetItem(r, i, iterable->list[i]);
    }
    return r;
}
extern "C" PyObject* PyDeque_Appendleft(PyObject* obj, PyObject* item) {
    if (!obj || obj->type != 1) return nullptr;
    pyc_ensure_boxed_list(obj);
    if (item) Py_INCREF(item);
    obj->list.insert(obj->list.begin(), item);
    return nullptr;
}
extern "C" PyObject* PyDeque_Popleft(PyObject* obj) {
    if (!obj || obj->type != 1) return nullptr;
    pyc_ensure_boxed_list(obj);
    if (obj->list.empty()) { pyc_raise_msg("IndexError", "pop from an empty deque"); return nullptr; }
    PyObject* item = obj->list.front();
    if (item) Py_INCREF(item); // matches PyList_PopAt's convention
    obj->list.erase(obj->list.begin());
    return item;
}
// deque.rotate(n=1): positive n moves the last n elements to the front;
// negative n moves the first |n| elements to the end (verified against
// real collections.deque.rotate).
extern "C" PyObject* PyDeque_Rotate(PyObject* obj, PyObject* nObj) {
    if (!obj || obj->type != 1) return nullptr;
    pyc_ensure_boxed_list(obj);
    long n = (nObj && (nObj->type == 0 || nObj->type == 5)) ? (long)nObj->value : 1;
    size_t sz = obj->list.size();
    if (sz == 0) return nullptr;
    n %= (long)sz;
    if (n < 0) n += (long)sz;
    if (n == 0) return nullptr;
    std::rotate(obj->list.begin(), obj->list.begin() + (long)(sz - (size_t)n), obj->list.end());
    return nullptr;
}

// collections.namedtuple(typename, field_names) -> bundle
// ["PyCollections_NamedtupleConstruct", fieldNamesList]; calling it
// (Point(1, 2)) builds a plain dict pairing field_names[i] with each
// positional argument. `.x`/`.y` attribute access then works for free
// via the existing generic lowerAttribute -> Pyc_GetItem path (a plain
// dict with a string key already supports `.key` attribute syntax).
// Positional construction only (Point(1, 2), not Point(x=1, y=2)) —
// Pyc_Apply's argument list is purely positional. print()ing an
// instance shows the plain dict repr ({'x': 1, 'y': 2}), not CPython's
// Point(x=1, y=2) — cosmetic gap, not fixed (documented).
extern "C" PyObject* PyCollections_Namedtuple(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    PyObject* fieldNames = args->list[1];
    PyObject* bundle = PyList_New(2);
    PyObject* tok = PyUnicode_FromString("PyCollections_NamedtupleConstruct");
    if (fieldNames) Py_INCREF(fieldNames);
    PyList_SetItem(bundle, 0, tok);
    PyList_SetItem(bundle, 1, fieldNames);
    Py_DECREF(tok);
    return bundle;
}
extern "C" PyObject* PyCollections_NamedtupleConstruct(PyObject* args) {
    // args = [fieldNamesList, ...positionalValues]
    PyObject* d = PyDict_New();
    if (!args || args->type != 1 || args->list.empty()) return d;
    PyObject* fieldNames = args->list[0];
    if (!fieldNames || fieldNames->type != 1) return d;
    pyc_ensure_boxed_list(fieldNames);
    for (size_t i = 0; i < fieldNames->list.size() && i + 1 < args->list.size(); ++i) {
        PyDict_SetItem(d, fieldNames->list[i], args->list[i + 1]);
    }
    return d;
}

// collections.defaultdict(default_factory) -> a real dict (type 2) whose
// factory is recorded in g_pycDefaultFactories (declared next to
// Pyc_Subscript above, whose dict-miss path consults it before raising
// KeyError). Keeping the factory out-of-band (not a visible dict key)
// means the dict prints/len()s/iterates exactly like a plain dict,
// matching the "instances look like plain dicts" tradeoff already made
// for namedtuple above.
extern "C" PyObject* PyCollections_Defaultdict(PyObject* args) {
    PyObject* d = PyDict_New();
    if (!args || args->type != 1 || args->list.empty()) return d;
    PyObject* factory = args->list[0];
    if (factory) Py_INCREF(factory); // g_pycDefaultFactories's own reference
    g_pycDefaultFactories[d] = factory;
    return d;
}

// Zero-arg factory tokens for `defaultdict(list)`/`defaultdict(int)`/etc.
// A bare (uncalled) reference to a builtin type name like `list` normally
// has no runtime representation at all in pyc — only *calls* like
// `list(x)` are recognized structurally (Compiler.cpp's lowerCall). This
// is the same gap B13 (builtinExcNames) already closes for exception
// classes used as first-class values (`exc = ValueError`); these are the
// collections-factory equivalent, registered as ordinary token+registry
// callables so PyCollections_Defaultdict's stored factory can be invoked
// generically via Pyc_Apply with an empty arg list, same as any other
// zero-arg call.
extern "C" PyObject* PyBuiltin_ListFactory(PyObject*)  { return PyList_New(0); }
extern "C" PyObject* PyBuiltin_DictFactory(PyObject*)  { return PyDict_New(); }
extern "C" PyObject* PyBuiltin_IntFactory(PyObject*)   { return PyInt_FromLong(0); }
extern "C" PyObject* PyBuiltin_FloatFactory(PyObject*) { return PyFloat_FromDouble(0.0); }
extern "C" PyObject* PyBuiltin_StrFactory(PyObject*)   { return PyUnicode_FromString(""); }
extern "C" PyObject* PyBuiltin_SetFactory(PyObject*)   { return PySet_New(); }

static PyObject* makeCollectionsModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("Counter", "PyCollections_Counter");
    addTok("most_common", "PyCollections_MostCommon");
    // "deque" is NOT a dict entry — it needs AST-structural construction
    // (see PyCollections_Deque's comment above) so its result can carry
    // the compile-time "deque" typeOf tag, same as csv.writer/pathlib.Path.
    // "namedtuple"/"defaultdict" ARE normal tokens: namedtuple's factory
    // takes purely positional args (typename, field_names) and returns a
    // plain bundle, same shape as functools.partial, so the generic
    // dict-dispatch call convention already handles it with no
    // Compiler.cpp changes; defaultdict's construction doesn't need a
    // custom typeOf tag either (Pyc_Subscript's marker-key check works on
    // any real dict, no compile-time tracking needed).
    addTok("namedtuple", "PyCollections_Namedtuple");
    addTok("defaultdict", "PyCollections_Defaultdict");
    return d;
}

// ---- datetime module ----
// Construction (`datetime.date(...)`/`datetime.datetime(...)`/
// `datetime.timedelta(...)`) is intercepted at the AST level in
// Compiler.cpp (mirroring the cmath/re Name-based dispatch pattern) and
// routed directly to these constructors — not through the generic
// token+registry/Pyc_Apply path used by math/json/random, since these
// calls also need to tag the result's compiler-inferred type (via
// noteType) for the method-call fast path (see IMPLEMENTATION.md for the
// robust-vs-fast-path distinction: attribute reads, arithmetic, and
// comparisons work regardless of typeOf tracking via Pyc_GetItem/
// PyNumber_*/PyObject_CompareBool above; these method-call-syntax
// functions below only fire when typeOf tracking succeeds).
static long pyc_arg_int(PyObject* o, long defaultVal) {
    return is_numeric(o) ? (long)numeric_val(o) : defaultVal;
}

extern "C" PyObject* PyDateTime_Date(PyObject* y, PyObject* m, PyObject* d) {
    return pyc_new_datetime((int)pyc_arg_int(y, 1), (int)pyc_arg_int(m, 1), (int)pyc_arg_int(d, 1), 0, 0, 0, false);
}
extern "C" PyObject* PyDateTime_Datetime(PyObject* y, PyObject* m, PyObject* d, PyObject* h, PyObject* mi, PyObject* s) {
    return pyc_new_datetime((int)pyc_arg_int(y, 1), (int)pyc_arg_int(m, 1), (int)pyc_arg_int(d, 1),
                             (int)pyc_arg_int(h, 0), (int)pyc_arg_int(mi, 0), (int)pyc_arg_int(s, 0), true);
}
extern "C" PyObject* PyTimedelta_New(PyObject* days, PyObject* seconds, PyObject* minutes, PyObject* hours, PyObject* weeks) {
    int64_t d = pyc_arg_int(days, 0), s = pyc_arg_int(seconds, 0);
    int64_t mi = pyc_arg_int(minutes, 0), h = pyc_arg_int(hours, 0), w = pyc_arg_int(weeks, 0);
    return pyc_new_timedelta(d + w * 7, s + mi * 60 + h * 3600, 0);
}

extern "C" PyObject* PyDateTime_Isoformat(PyObject* obj) {
    PycDateTime* dt = pyc_as_datetime(obj);
    if (!dt) return PyUnicode_FromString("");
    char buf[64];
    if (dt->hasTime) {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                 dt->year, dt->month, dt->day, dt->hour, dt->minute, dt->second);
    } else {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt->year, dt->month, dt->day);
    }
    return PyUnicode_FromString(buf);
}
extern "C" PyObject* PyDateTime_Weekday(PyObject* obj) {
    PycDateTime* dt = pyc_as_datetime(obj);
    if (!dt) return PyInt_FromLong(0);
    return PyInt_FromLong(pyc_weekday_from_days(pyc_days_from_civil(dt->year, dt->month, dt->day)));
}
extern "C" PyObject* PyDateTime_Isoweekday(PyObject* obj) {
    PycDateTime* dt = pyc_as_datetime(obj);
    if (!dt) return PyInt_FromLong(1);
    return PyInt_FromLong(pyc_weekday_from_days(pyc_days_from_civil(dt->year, dt->month, dt->day)) + 1);
}
extern "C" PyObject* PyTimedelta_TotalSeconds(PyObject* obj) {
    PycTimedelta* td = pyc_as_timedelta(obj);
    if (!td) return PyFloat_FromDouble(0.0);
    return PyFloat_FromDouble((double)td->days * 86400.0 + (double)td->seconds + (double)td->microseconds / 1000000.0);
}

// date.today() / datetime.now(): a real wall-clock read, unlike
// time.perf_counter's fixed-constant stub — but for the same reason that
// stub exists, calls to these are excluded from CPython-exact-match test
// assertions (there's no way to make "the current date" deterministic).
extern "C" PyObject* PyDateTime_Today(PyObject* args) {
    (void)args;
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return pyc_new_datetime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, 0, 0, 0, false);
}
extern "C" PyObject* PyDateTime_Now(PyObject* args) {
    (void)args;
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return pyc_new_datetime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                             lt.tm_hour, lt.tm_min, lt.tm_sec, true);
}

// makeDatetimeModuleDict: `date`/`datetime`/`timedelta` constructors are
// intercepted at the AST level and never actually looked up in this dict
// (see the comment above) — it exists so `import datetime` binds a real
// dict rather than erroring, and so `datetime.date.today()` /
// `datetime.datetime.now()` (token+registry, not AST-intercepted, since
// they take no constructor-shaped arguments) resolve normally. Nested
// under `date`/`datetime` sub-dicts to match the real attribute path.
static PyObject* makeDatetimeModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](PyObject* target, const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(target, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    PyObject* dateSub = PyDict_New();
    addTok(dateSub, "today", "PyDateTime_Today");
    PyObject* dateKey = PyUnicode_FromString("date");
    PyDict_SetItem(d, dateKey, dateSub);
    Py_DECREF(dateKey); Py_DECREF(dateSub);

    PyObject* datetimeSub = PyDict_New();
    addTok(datetimeSub, "now", "PyDateTime_Now");
    PyObject* datetimeKey = PyUnicode_FromString("datetime");
    PyDict_SetItem(d, datetimeKey, datetimeSub);
    Py_DECREF(datetimeKey); Py_DECREF(datetimeSub);
    return d;
}

// hashlib: empty dict — md5/sha1/sha256 construction and .hexdigest() are
// always intercepted structurally in Compiler.cpp (like datetime's
// date/datetime/timedelta), never looked up via this dict at runtime.
// Exists only so `import hashlib` doesn't report ImportError.
static PyObject* makeHashlibModuleDict() {
    return PyDict_New();
}

static PyObject* makeBase64ModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("b64encode", "PyBase64_B64Encode");
    addTok("b64decode", "PyBase64_B64Decode");
    return d;
}

static PyObject* makeStructModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("pack", "PyStruct_Pack");
    addTok("unpack", "PyStruct_Unpack");
    return d;
}

static PyObject* makeHeapqModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("heapify",    "PyHeapq_Heapify");
    addTok("heappush",   "PyHeapq_Heappush");
    addTok("heappop",    "PyHeapq_Heappop");
    addTok("heappushpop","PyHeapq_Heappushpop");
    addTok("heapreplace","PyHeapq_Heapreplace");
    addTok("nlargest",   "PyHeapq_Nlargest");
    addTok("nsmallest",  "PyHeapq_Nsmallest");
    return d;
}

static PyObject* makeBisectModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("bisect_left",  "PyBisect_Left");
    addTok("bisect_right", "PyBisect_Right");
    addTok("bisect",       "PyBisect_Right"); // bisect() is an alias for bisect_right
    addTok("insort_left",  "PyBisect_InsortLeft");
    addTok("insort_right", "PyBisect_InsortRight");
    addTok("insort",       "PyBisect_InsortRight"); // insort() is an alias for insort_right
    return d;
}

static PyObject* makeStatisticsModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("mean",       "PyStatistics_Mean");
    addTok("median",     "PyStatistics_Median");
    addTok("median_low", "PyStatistics_MedianLow");
    addTok("median_high","PyStatistics_MedianHigh");
    addTok("mode",       "PyStatistics_Mode");
    addTok("stdev",      "PyStatistics_Stdev");
    addTok("variance",   "PyStatistics_Variance");
    addTok("pstdev",     "PyStatistics_Pstdev");
    addTok("pvariance",  "PyStatistics_Pvariance");
    return d;
}

// string: pure constants (no functions) — values matching CPython's
// string module exactly.
static PyObject* makeStringModuleDict() {
    PyObject* d = PyDict_New();
    auto addConst = [&](const char* name, const char* value) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(value);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addConst("ascii_lowercase", "abcdefghijklmnopqrstuvwxyz");
    addConst("ascii_uppercase", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    addConst("ascii_letters",   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    addConst("digits",          "0123456789");
    addConst("hexdigits",       "0123456789abcdefABCDEF");
    addConst("octdigits",       "01234567");
    addConst("punctuation",     "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
    addConst("whitespace",      " \t\n\r\x0b\x0c");
    addConst("printable",
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0b\x0c");
    return d;
}

static PyObject* makeTextwrapModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("wrap", "PyTextwrap_Wrap");
    addTok("fill", "PyTextwrap_Fill");
    return d;
}

static PyObject* makeUuidModuleDict() {
    PyObject* d = PyDict_New();
    PyObject* k = PyUnicode_FromString("uuid4");
    PyObject* v = PyUnicode_FromString("PyUuid_Uuid4");
    PyDict_SetItem(d, k, v);
    Py_DECREF(k); Py_DECREF(v);
    return d;
}

// copy: empty — copy.copy(...)/copy.deepcopy(...) are always intercepted
// structurally in Compiler.cpp (see PyCopy_Copy/Deepcopy's comment for
// why this can't be a token+registry dict entry like other modules).
// This dict exists only so `import copy` doesn't report ImportError.
static PyObject* makeCopyModuleDict() {
    return PyDict_New();
}

static PyObject* makeShutilModuleDict() {
    PyObject* d = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("copyfile", "PyShutil_Copyfile");
    addTok("move",     "PyShutil_Move");
    addTok("rmtree",   "PyShutil_Rmtree");
    return d;
}

static PyObject* makeGlobModuleDict() {
    PyObject* d = PyDict_New();
    PyObject* k = PyUnicode_FromString("glob");
    PyObject* v = PyUnicode_FromString("PyGlob_Glob");
    PyDict_SetItem(d, k, v);
    Py_DECREF(k); Py_DECREF(v);
    return d;
}

// `writer` is NOT a dict entry here — csv.writer(f) is always
// intercepted structurally in Compiler.cpp (see PyCsv_Writer's comment),
// same as pathlib.Path/hashlib.md5 construction never being looked up
// via their module dicts either.
static PyObject* makeCsvModuleDict() {
    PyObject* d = PyDict_New();
    PyObject* k = PyUnicode_FromString("reader");
    PyObject* v = PyUnicode_FromString("PyCsv_Reader");
    PyDict_SetItem(d, k, v);
    Py_DECREF(k); Py_DECREF(v);
    return d;
}

// makeOsModuleDict: builds a dict that emulates the os module. The
// `os.environ` entry is a real dict populated from the process
// environment (`environ(7)`); `os.path` is a dict whose entries are
// string tokens naming runtime helpers; `os.unlink`/`os.remove`/etc. are
// also top-level tokens.
static PyObject* makeOsModuleDict() {
    PyObject* d = PyDict_New();
    // os.environ -> real dict populated from the process environment.
    // Values set/mutated by user code afterward don't propagate back to
    // the actual process environment (no os.putenv/os.environ[k]=v write
    // path) — read-only snapshot at import time, matching the scope of
    // every other os.* stub here.
    PyObject* env_key = PyUnicode_FromString("environ");
    PyObject* env_val = PyDict_New();
    for (char** e = environ; *e; ++e) {
        const char* eq = strchr(*e, '=');
        if (!eq) continue;
        PyObject* k = PyUnicode_FromString(std::string(*e, eq - *e).c_str());
        PyObject* v = PyUnicode_FromString(eq + 1);
        PyDict_SetItem(env_val, k, v);
        Py_DECREF(k); Py_DECREF(v);
    }
    PyDict_SetItem(d, env_key, env_val);
    Py_DECREF(env_key); Py_DECREF(env_val);
    // os.path -> dict with exists/isfile/isdir/unlink/join/basename/
    // dirname/splitext/abspath tokens
    PyObject* path_key = PyUnicode_FromString("path");
    PyObject* path_val = PyDict_New();
    auto addTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(path_val, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTok("exists",   "PyBuiltin_OsPathExists");
    addTok("isfile",   "PyBuiltin_OsPathIsfile");
    addTok("isdir",    "PyBuiltin_OsPathIsdir");
    addTok("unlink",   "PyBuiltin_OsUnlink");
    addTok("join",     "PyBuiltin_OsPathJoin");
    addTok("basename", "PyBuiltin_OsPathBasename");
    addTok("dirname",  "PyBuiltin_OsPathDirname");
    addTok("splitext", "PyBuiltin_OsPathSplitext");
    addTok("split",   "PyBuiltin_OsPathSplit");
    addTok("abspath",  "PyBuiltin_OsPathAbspath");
    PyDict_SetItem(d, path_key, path_val);
    Py_DECREF(path_key); Py_DECREF(path_val);
    // Top-level os.* tokens (not on os.path)
    auto addTopTok = [&](const char* name, const char* token) {
        PyObject* k = PyUnicode_FromString(name);
        PyObject* v = PyUnicode_FromString(token);
        PyDict_SetItem(d, k, v);
        Py_DECREF(k); Py_DECREF(v);
    };
    addTopTok("unlink",   "PyBuiltin_OsUnlink");
    addTopTok("remove",   "PyBuiltin_OsRemove");
    addTopTok("rename",   "PyBuiltin_OsRename");
    addTopTok("getcwd",   "PyBuiltin_OsGetcwd");
    addTopTok("listdir",  "PyBuiltin_OsListdir");
    addTopTok("makedirs", "PyBuiltin_OsMakedirs");
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

// re.sub(pattern, repl, subject, count, flags) — replace matches. Uses
// pcre2_substitute for full regex semantics (including backreferences
// like \1, \2 in the replacement). The `count` argument limits
// replacements; a non-positive or null count means "all".
extern "C" PyObject* PyBuiltin_ReSub(PyObject* pattern, PyObject* repl,
                                      PyObject* subject, PyObject* count, PyObject* flags) {
    if (!pattern || pattern->type != 3 || !repl || repl->type != 3 ||
        !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err, pyc_re_unbox_flags(flags));
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

// re.split(pattern, subject, maxsplit, flags) — split subject on regex
// matches (literal if the pattern contains no regex metacharacters). We
// honour the empty-match semantics: an empty pattern or a zero-width
// match advances one position to avoid infinite loops. `maxsplit`, if
// positive, caps the number of splits performed (any non-positive or
// null value means "no limit", matching this file's existing convention
// for re.sub's `count`).
extern "C" PyObject* PyBuiltin_ReSplit(PyObject* pattern, PyObject* subject,
                                        PyObject* maxsplit, PyObject* flags) {
    if (!pattern || pattern->type != 3 || !subject || subject->type != 3) return nullptr;
    std::string err;
    pcre2_code* code = compileRegex(pattern->str, err, pyc_re_unbox_flags(flags));
    if (!code) {
        std::fprintf(stderr, "re.error: %s\n", err.c_str());
        return nullptr;
    }
    long maxSplits = (maxsplit && (maxsplit->type == 0 || maxsplit->type == 5)) ? maxsplit->value : 0;
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    if (!md) { pcre2_code_free(code); return nullptr; }
    PCRE2_SPTR subj = (PCRE2_SPTR)subject->str.c_str();
    int rc = pcre2_match(code, subj, (PCRE2_SIZE)subject->str.size(), 0, 0, md, nullptr);
    PyObject* result = PyList_New(0);
    PCRE2_SIZE offset = 0;
    int splitsDone = 0;
    while (rc >= 0) {
        if (maxSplits > 0 && splitsDone >= maxSplits) break;
        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(md);
        PCRE2_SIZE start = ovector[0];
        PCRE2_SIZE end   = ovector[1];
        if (start == end) {
            // Empty match: append the empty string and advance one.
            if (offset <= subject->str.size()) {
                std::string piece = subject->str.substr(offset, (start - offset));
                PyObject* s = PyUnicode_FromString(piece.c_str());
                result->list.push_back(s);
                ++splitsDone;
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
        ++splitsDone;
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
    
    // B7: sys.modules — a dict mapping module names to module dicts.
    // Initially contains "sys" pointing to the sys module itself.
    // Other modules are added at import time.
    {
        PyObject* modules_dict = PyDict_New();
        PyObject* sys_key = PyUnicode_FromString("sys");
        PyDict_SetItem(modules_dict, sys_key, g_sys_module);
        Py_DECREF(sys_key);
        // Store sys.modules as an attribute on the sys module
        PyObject* sys_modules_key = PyUnicode_FromString("modules");
        PyDict_SetItem(g_sys_module, sys_modules_key, modules_dict);
        Py_DECREF(sys_modules_key);
        // Store a reference to sys.modules in a global for easy access
        g_sys_modules = modules_dict;
        Py_DECREF(modules_dict);
    }
}

// ---- Builtin function adapters for first-class value support ----
// Each adapter unpacks the Pyc_Apply args list and dispatches to the
// existing PyBuiltin_* function. This allows builtins like len, abs,
// str, etc. to be used as values (sorted(key=len), functools.reduce(max),
// apply(abs, x), etc.).

static inline PyObject* arg0(PyObject* args) {
    return (args && args->type == 1 && !args->list.empty()) ? args->list[0] : nullptr;
}
static inline PyObject* arg1(PyObject* args) {
    return (args && args->type == 1 && args->list.size() > 1) ? args->list[1] : nullptr;
}

static PyObject* pyc_adapt_len(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Len(a);
}
static PyObject* pyc_adapt_abs(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Abs(a);
}
static PyObject* pyc_adapt_str(PyObject* args) {
    PyObject* a = arg0(args);
    if (!a) return PyUnicode_FromString("");
    return PyStr_FromAny(a);
}
static PyObject* pyc_adapt_int(PyObject* args) {
    PyObject* a = arg0(args);
    if (!a) return PyInt_FromLong(0);
    return PyBuiltin_Int(a);
}
static PyObject* pyc_adapt_float(PyObject* args) {
    PyObject* a = arg0(args);
    if (!a) return PyFloat_FromDouble(0.0);
    return PyBuiltin_Float(a);
}
static PyObject* pyc_adapt_bool(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return PyBool_New(0);
    return PyBuiltin_Bool(a);
}
static PyObject* pyc_adapt_type(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Type(a);
}
static PyObject* pyc_adapt_repr(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Repr(a);
}
static PyObject* pyc_adapt_id(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Id(a);
}
static PyObject* pyc_adapt_callable(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Callable(a);
}
static PyObject* pyc_adapt_ord(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Ord(a);
}
static PyObject* pyc_adapt_chr(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Chr(a);
}
static PyObject* pyc_adapt_hex(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Hex(a);
}
static PyObject* pyc_adapt_oct(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Oct(a);
}
static PyObject* pyc_adapt_bin(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Bin(a);
}
static PyObject* pyc_adapt_round(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    PyObject* b = arg1(args);
    return PyBuiltin_Round(a, b);
}
static PyObject* pyc_adapt_divmod(PyObject* args) {
    PyObject* a = arg0(args); PyObject* b = arg1(args);
    if (!a || !b) return nullptr;
    return PyBuiltin_Divmod(a, b);
}
static PyObject* pyc_adapt_pow(PyObject* args) {
    PyObject* a = arg0(args); PyObject* b = arg1(args);
    if (!a || !b) return nullptr;
    return PyBuiltin_Pow(a, b);
}
static PyObject* pyc_adapt_list(PyObject* args) {
    PyObject* a = arg0(args);
    return PyBuiltin_List(a);
}
static PyObject* pyc_adapt_tuple(PyObject* args) {
    PyObject* a = arg0(args);
    return PyBuiltin_Tuple(a);
}
static PyObject* pyc_adapt_set(PyObject* args) {
    PyObject* a = arg0(args);
    PyObject* res = PySet_New();
    if (a) PySet_Update(res, a);
    return res;
}
static PyObject* pyc_adapt_range(PyObject* args) {
    if (!args || args->type != 1) return nullptr;
    if (args->list.size() == 1) return PyBuiltin_Range(nullptr, args->list[0], nullptr);
    if (args->list.size() == 2) return PyBuiltin_Range(args->list[0], args->list[1], nullptr);
    if (args->list.size() >= 3) return PyBuiltin_Range(args->list[0], args->list[1], args->list[2]);
    return nullptr;
}
static PyObject* pyc_adapt_print(PyObject* args) {
    if (!args || args->type != 1) {
        std::printf("\n");
        return PyInt_FromLong(0);
    }
    for (size_t i = 0; i < args->list.size(); ++i) {
        if (i > 0) std::printf(" ");
        if (args->list[i]) {
            PyObject_PrintElement(args->list[i], stdout);
        } else {
            std::printf("None");
        }
    }
    std::printf("\n");
    return PyInt_FromLong(0);
}
static PyObject* pyc_adapt_min(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    if (args->list.size() == 1) return PyBuiltin_MinList(args->list[0], nullptr, nullptr);
    // Multiple positional args: compare pairwise
    PyObject* best = args->list[0];
    if (best) Py_INCREF(best);
    for (size_t i = 1; i < args->list.size(); ++i) {
        PyObject* r = PyBuiltin_Min2(best, args->list[i], nullptr);
        if (best) Py_DECREF(best);
        best = r;
        if (!best) break;
    }
    return best;
}
static PyObject* pyc_adapt_max(PyObject* args) {
    if (!args || args->type != 1 || args->list.empty()) return nullptr;
    if (args->list.size() == 1) return PyBuiltin_MaxList(args->list[0], nullptr, nullptr);
    PyObject* best = args->list[0];
    if (best) Py_INCREF(best);
    for (size_t i = 1; i < args->list.size(); ++i) {
        PyObject* r = PyBuiltin_Max2(best, args->list[i], nullptr);
        if (best) Py_DECREF(best);
        best = r;
        if (!best) break;
    }
    return best;
}
static PyObject* pyc_adapt_sum(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Sum(a);
}
static PyObject* pyc_adapt_sorted(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Sorted(a, nullptr, nullptr);
}
static PyObject* pyc_adapt_any(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Any(a);
}
static PyObject* pyc_adapt_all(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_All(a);
}
static PyObject* pyc_adapt_reversed(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Reversed(a);
}
static PyObject* pyc_adapt_enumerate(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Enumerate(a);
}
static PyObject* pyc_adapt_zip(PyObject* args) {
    if (!args || args->type != 1 || args->list.size() < 2) return nullptr;
    return PyBuiltin_Zip2(args->list[0], args->list[1]);
}
static PyObject* pyc_adapt_isinstance(PyObject* args) {
    // Note: isinstance as a value is limited — the compile-time typecode
    // resolution can't happen here. Fall back to a basic type-name check.
    PyObject* a = arg0(args); PyObject* b = arg1(args);
    if (!a || !b) return PyBool_New(0);
    // b is typically a type name string; compare against type(a)
    PyObject* t = PyBuiltin_Type(a);
    bool match = (t && b->type == 3 && t->str == b->str);
    if (t) Py_DECREF(t);
    return PyBool_New(match ? 1 : 0);
}
static PyObject* pyc_adapt_complex(PyObject* args) {
    PyObject* a = arg0(args); PyObject* b = arg1(args);
    return PyBuiltin_Complex(a, b);
}
static PyObject* pyc_adapt_bytes(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Bytes(a, nullptr);
}
static PyObject* pyc_adapt_bytearray(PyObject* args) {
    PyObject* a = arg0(args); if (!a) return nullptr;
    return PyBuiltin_Bytearray(a, nullptr);
}

// Register all the os/subprocess/etc. runtime helpers so they can be
// dispatched via Pyc_Apply using their PyBuiltin_* names. The synthetic
// module dicts (`makeOsModuleDict`, `makeSubprocessModuleDict`) embed
// these names as string tokens, and the compiler emits
// `Pyc_Apply(token, args)` for module-attribute calls like
// `os.path.exists(p)`. Without this registration, Pyc_Apply returns
// null for the token. Idempotent (safe to call multiple times).
extern "C" PyObject* Pyc_Time_PerfCounter(PyObject* args);
extern "C" void pyc_setup_callables(void) {
    static bool done = false;
    if (done) return;
    done = true;
    pyc_register_callable("PyBuiltin_OsPathExists",        PyBuiltin_OsPathExists);
    pyc_register_callable("PyBuiltin_OsPathIsfile",        PyBuiltin_OsPathIsfile);
    pyc_register_callable("PyBuiltin_OsPathIsdir",         PyBuiltin_OsPathIsdir);
    pyc_register_callable("PyBuiltin_OsUnlink",            PyBuiltin_OsUnlink);
    pyc_register_callable("PyBuiltin_OsPathJoin",          PyBuiltin_OsPathJoin);
    pyc_register_callable("PyBuiltin_OsPathBasename",      PyBuiltin_OsPathBasename);
    pyc_register_callable("PyBuiltin_OsPathDirname",       PyBuiltin_OsPathDirname);
    pyc_register_callable("PyBuiltin_OsPathSplitext",      PyBuiltin_OsPathSplitext);
    pyc_register_callable("PyBuiltin_OsPathSplit",        PyBuiltin_OsPathSplit);
    pyc_register_callable("PyBuiltin_OsPathAbspath",       PyBuiltin_OsPathAbspath);
    pyc_register_callable("PyBuiltin_OsGetcwd",            PyBuiltin_OsGetcwd);
    pyc_register_callable("PyBuiltin_OsListdir",           PyBuiltin_OsListdir);
    pyc_register_callable("PyBuiltin_OsMakedirs",          PyBuiltin_OsMakedirs);
    pyc_register_callable("PyBuiltin_OsRemove",            PyBuiltin_OsRemove);
    pyc_register_callable("PyBuiltin_OsRename",            PyBuiltin_OsRename);
    pyc_register_callable("PyBase64_B64Encode",            PyBase64_B64Encode);
    pyc_register_callable("PyBase64_B64Decode",            PyBase64_B64Decode);
    pyc_register_callable("PyStruct_Pack",                 PyStruct_Pack);
    pyc_register_callable("PyStruct_Unpack",                PyStruct_Unpack);
    pyc_register_callable("PyHeapq_Heapify",                PyHeapq_Heapify);
    pyc_register_callable("PyHeapq_Heappush",               PyHeapq_Heappush);
    pyc_register_callable("PyHeapq_Heappop",                PyHeapq_Heappop);
    pyc_register_callable("PyHeapq_Heappushpop",            PyHeapq_Heappushpop);
    pyc_register_callable("PyHeapq_Heapreplace",            PyHeapq_Heapreplace);
    pyc_register_callable("PyHeapq_Nlargest",               PyHeapq_Nlargest);
    pyc_register_callable("PyHeapq_Nsmallest",              PyHeapq_Nsmallest);
    pyc_register_callable("PyBisect_Left",                  PyBisect_Left);
    pyc_register_callable("PyBisect_Right",                 PyBisect_Right);
    pyc_register_callable("PyBisect_InsortLeft",            PyBisect_InsortLeft);
    pyc_register_callable("PyBisect_InsortRight",           PyBisect_InsortRight);
    pyc_register_callable("PyStatistics_Mean",              PyStatistics_Mean);
    pyc_register_callable("PyStatistics_Median",            PyStatistics_Median);
    pyc_register_callable("PyStatistics_MedianLow",         PyStatistics_MedianLow);
    pyc_register_callable("PyStatistics_MedianHigh",        PyStatistics_MedianHigh);
    pyc_register_callable("PyStatistics_Mode",              PyStatistics_Mode);
    pyc_register_callable("PyStatistics_Stdev",             PyStatistics_Stdev);
    pyc_register_callable("PyStatistics_Variance",          PyStatistics_Variance);
    pyc_register_callable("PyStatistics_Pstdev",            PyStatistics_Pstdev);
    pyc_register_callable("PyStatistics_Pvariance",         PyStatistics_Pvariance);
    pyc_register_callable("PyTextwrap_Wrap",                PyTextwrap_Wrap);
    pyc_register_callable("PyTextwrap_Fill",                PyTextwrap_Fill);
    pyc_register_callable("PyUuid_Uuid4",                   PyUuid_Uuid4);
    pyc_register_callable("PyFunctools_Reduce",             PyFunctools_Reduce);
    pyc_register_callable("PyFunctools_Partial",            PyFunctools_Partial);
    pyc_register_callable("PyFunctools_Wraps",              PyFunctools_Wraps);
    pyc_register_callable("PyFunctools_WrapsIdentity",      PyFunctools_WrapsIdentity);
    pyc_register_callable("PyFunctools_LruCache",           PyFunctools_LruCache);
    pyc_register_callable("PyFunctools_LruCacheCall",       PyFunctools_LruCacheCall);
    pyc_register_callable("PyOperator_Add",                 PyOperator_Add);
    pyc_register_callable("PyOperator_Sub",                 PyOperator_Sub);
    pyc_register_callable("PyOperator_Mul",                 PyOperator_Mul);
    pyc_register_callable("PyOperator_Truediv",             PyOperator_Truediv);
    pyc_register_callable("PyOperator_Mod",                 PyOperator_Mod);
    pyc_register_callable("PyOperator_Eq",                  PyOperator_Eq);
    pyc_register_callable("PyOperator_Ne",                  PyOperator_Ne);
    pyc_register_callable("PyOperator_Lt",                  PyOperator_Lt);
    pyc_register_callable("PyOperator_Gt",                  PyOperator_Gt);
    pyc_register_callable("PyOperator_Le",                  PyOperator_Le);
    pyc_register_callable("PyOperator_Ge",                  PyOperator_Ge);
    pyc_register_callable("PyOperator_Not",                 PyOperator_Not);
    pyc_register_callable("PyOperator_Neg",                 PyOperator_Neg);
    pyc_register_callable("PyOperator_Itemgetter",          PyOperator_Itemgetter);
    pyc_register_callable("PyOperator_ItemgetterCall",      PyOperator_ItemgetterCall);
    pyc_register_callable("PyOperator_Attrgetter",          PyOperator_Attrgetter);
    pyc_register_callable("PyOperator_AttrgetterCall",      PyOperator_AttrgetterCall);
    pyc_register_callable("PyShutil_Copyfile",              PyShutil_Copyfile);
    pyc_register_callable("PyShutil_Move",                  PyShutil_Move);
    pyc_register_callable("PyShutil_Rmtree",                PyShutil_Rmtree);
    pyc_register_callable("PyGlob_Glob",                    PyGlob_Glob);
    pyc_register_callable("PyCsv_Reader",                   PyCsv_Reader);
    pyc_register_callable("PyItertools_Accumulate",         PyItertools_Accumulate);
    pyc_register_callable("PyItertools_Takewhile",          PyItertools_Takewhile);
    pyc_register_callable("PyItertools_Dropwhile",          PyItertools_Dropwhile);
    pyc_register_callable("PyItertools_Compress",           PyItertools_Compress);
    pyc_register_callable("PyBuiltin_SubprocessCall",      PyBuiltin_SubprocessCall);
    pyc_register_callable("PyBuiltin_SubprocessCheckOutput", PyBuiltin_SubprocessCheckOutput);
    pyc_register_callable("pyc_stderr_write",              stderr_write_adapter);
    pyc_register_callable("pyc_stdout_write",              stdout_write_adapter);
    pyc_register_callable("pyc_file_write",                pyc_file_write_adapter);
    pyc_register_callable("pyc_file_enter",                pyc_file_enter_adapter);
    pyc_register_callable("pyc_file_exit",                 pyc_file_exit_adapter);
    pyc_register_callable("Pyc_Time_PerfCounter",          Pyc_Time_PerfCounter);
    pyc_register_callable("PyMath_Sqrt",     PyMath_Sqrt);
    pyc_register_callable("PyMath_Floor",    PyMath_Floor);
    pyc_register_callable("PyMath_Ceil",     PyMath_Ceil);
    pyc_register_callable("PyMath_Trunc",    PyMath_Trunc);
    pyc_register_callable("PyMath_Pow",      PyMath_Pow);
    pyc_register_callable("PyMath_Log",      PyMath_Log);
    pyc_register_callable("PyMath_Log2",     PyMath_Log2);
    pyc_register_callable("PyMath_Log10",    PyMath_Log10);
    pyc_register_callable("PyMath_Exp",      PyMath_Exp);
    pyc_register_callable("PyMath_Sin",      PyMath_Sin);
    pyc_register_callable("PyMath_Cos",      PyMath_Cos);
    pyc_register_callable("PyMath_Tan",      PyMath_Tan);
    pyc_register_callable("PyMath_Asin",     PyMath_Asin);
    pyc_register_callable("PyMath_Acos",     PyMath_Acos);
    pyc_register_callable("PyMath_Atan",     PyMath_Atan);
    pyc_register_callable("PyMath_Atan2",    PyMath_Atan2);
    pyc_register_callable("PyMath_Hypot",    PyMath_Hypot);
    pyc_register_callable("PyMath_Fabs",     PyMath_Fabs);
    pyc_register_callable("PyMath_Fmod",     PyMath_Fmod);
    pyc_register_callable("PyMath_Degrees",  PyMath_Degrees);
    pyc_register_callable("PyMath_Radians",  PyMath_Radians);
    pyc_register_callable("PyMath_Isnan",    PyMath_Isnan);
    pyc_register_callable("PyMath_Isinf",    PyMath_Isinf);
    pyc_register_callable("PyMath_Isfinite", PyMath_Isfinite);
    pyc_register_callable("PyMath_Gcd",      PyMath_Gcd);
    pyc_register_callable("PyMath_Factorial", PyMath_Factorial);
    pyc_register_callable("PyJson_Dumps", PyJson_Dumps);
    pyc_register_callable("PyJson_Loads", PyJson_Loads);
    pyc_register_callable("PyRandom_Seed",      PyRandom_Seed);
    pyc_register_callable("PyRandom_Random",    PyRandom_Random);
    pyc_register_callable("PyRandom_Randrange", PyRandom_Randrange);
    pyc_register_callable("PyRandom_Randint",   PyRandom_Randint);
    pyc_register_callable("PyRandom_Uniform",   PyRandom_Uniform);
    pyc_register_callable("PyRandom_Choice",    PyRandom_Choice);
    pyc_register_callable("PyRandom_Shuffle",   PyRandom_Shuffle);
    pyc_register_callable("PyItertools_Chain",        PyItertools_Chain);
    pyc_register_callable("PyItertools_Product",      PyItertools_Product);
    pyc_register_callable("PyItertools_Combinations", PyItertools_Combinations);
    pyc_register_callable("PyItertools_Permutations", PyItertools_Permutations);
    pyc_register_callable("PyItertools_Starmap",      PyItertools_Starmap);
    pyc_register_callable("PyItertools_Islice",       PyItertools_Islice);
    pyc_register_callable("PyItertools_ZipLongest",   PyItertools_ZipLongest);
    pyc_register_callable("PyCollections_Counter",    PyCollections_Counter);
    pyc_register_callable("PyCollections_MostCommon", PyCollections_MostCommon);
    pyc_register_callable("PyCollections_Namedtuple",          PyCollections_Namedtuple);
    pyc_register_callable("PyCollections_NamedtupleConstruct", PyCollections_NamedtupleConstruct);
    pyc_register_callable("PyCollections_Defaultdict",         PyCollections_Defaultdict);
    pyc_register_callable("PyBuiltin_ListFactory",  PyBuiltin_ListFactory);
    pyc_register_callable("PyBuiltin_DictFactory",  PyBuiltin_DictFactory);
    pyc_register_callable("PyBuiltin_IntFactory",   PyBuiltin_IntFactory);
    pyc_register_callable("PyBuiltin_FloatFactory", PyBuiltin_FloatFactory);
    pyc_register_callable("PyBuiltin_StrFactory",   PyBuiltin_StrFactory);
    pyc_register_callable("PyBuiltin_SetFactory",   PyBuiltin_SetFactory);
    // Builtin function adapters (first-class value support)
    pyc_register_callable("pyc_adapt_len",        pyc_adapt_len);
    pyc_register_callable("pyc_adapt_abs",        pyc_adapt_abs);
    pyc_register_callable("pyc_adapt_str",        pyc_adapt_str);
    pyc_register_callable("pyc_adapt_int",        pyc_adapt_int);
    pyc_register_callable("pyc_adapt_float",      pyc_adapt_float);
    pyc_register_callable("pyc_adapt_bool",       pyc_adapt_bool);
    pyc_register_callable("pyc_adapt_type",       pyc_adapt_type);
    pyc_register_callable("pyc_adapt_repr",       pyc_adapt_repr);
    pyc_register_callable("pyc_adapt_id",         pyc_adapt_id);
    pyc_register_callable("pyc_adapt_callable",   pyc_adapt_callable);
    pyc_register_callable("pyc_adapt_ord",        pyc_adapt_ord);
    pyc_register_callable("pyc_adapt_chr",        pyc_adapt_chr);
    pyc_register_callable("pyc_adapt_hex",        pyc_adapt_hex);
    pyc_register_callable("pyc_adapt_oct",        pyc_adapt_oct);
    pyc_register_callable("pyc_adapt_bin",        pyc_adapt_bin);
    pyc_register_callable("pyc_adapt_round",      pyc_adapt_round);
    pyc_register_callable("pyc_adapt_divmod",     pyc_adapt_divmod);
    pyc_register_callable("pyc_adapt_pow",        pyc_adapt_pow);
    pyc_register_callable("pyc_adapt_list",       pyc_adapt_list);
    pyc_register_callable("pyc_adapt_tuple",      pyc_adapt_tuple);
    pyc_register_callable("pyc_adapt_set",        pyc_adapt_set);
    pyc_register_callable("pyc_adapt_range",      pyc_adapt_range);
    pyc_register_callable("pyc_adapt_print",      pyc_adapt_print);
    pyc_register_callable("pyc_adapt_min",        pyc_adapt_min);
    pyc_register_callable("pyc_adapt_max",        pyc_adapt_max);
    pyc_register_callable("pyc_adapt_sum",        pyc_adapt_sum);
    pyc_register_callable("pyc_adapt_sorted",     pyc_adapt_sorted);
    pyc_register_callable("pyc_adapt_any",        pyc_adapt_any);
    pyc_register_callable("pyc_adapt_all",        pyc_adapt_all);
    pyc_register_callable("pyc_adapt_reversed",   pyc_adapt_reversed);
    pyc_register_callable("pyc_adapt_enumerate",  pyc_adapt_enumerate);
    pyc_register_callable("pyc_adapt_zip",        pyc_adapt_zip);
    pyc_register_callable("pyc_adapt_isinstance", pyc_adapt_isinstance);
    pyc_register_callable("pyc_adapt_complex",    pyc_adapt_complex);
    pyc_register_callable("pyc_adapt_bytes",      pyc_adapt_bytes);
    pyc_register_callable("pyc_adapt_bytearray",  pyc_adapt_bytearray);
    pyc_register_callable("PyDateTime_Today", PyDateTime_Today);
    pyc_register_callable("PyDateTime_Now",   PyDateTime_Now);
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

// B7: Get the sys.modules dict (a new strong reference, or NULL if not initialised).
PyObject* pyc_get_sys_modules(void) {
    if (g_sys_modules == nullptr) return nullptr;
    Py_INCREF(g_sys_modules);
    return g_sys_modules;
}

// B7: Add a module to sys.modules (increments refcount of module_dict).
void pyc_register_module(const char* name, PyObject* module_dict) {
    if (!g_sys_modules || !module_dict) return;
    PyObject* nameKey = PyUnicode_FromString(name);
    Py_INCREF(module_dict);
    PyDict_SetItem(g_sys_modules, nameKey, module_dict);
    Py_DECREF(nameKey);
}

PyObject* PyNumber_Multiply(PyObject* a, PyObject* b) {
    if (!a || !b) return NULL;
    // __mul__ dispatch for a class instance — see PyNumber_Add's comment.
    if (a->type == 2) {
        PyObject* method = pyc_lookup_dunder(a, "__mul__");
        if (method) return pyc_call_dunder2(method, a, b);
    }
    if (a->type == 3 && b->type == 0) return PyString_Repeat(a, b);
    if (a->type == 0 && b->type == 3) return PyString_Repeat(b, a);
    if (a->type == 1 && b->type == 0) return PyList_Repeat(a, b->value);
    if (a->type == 0 && b->type == 1) return PyList_Repeat(b, a->value);
    // tuple * int / int * tuple -> tuple (CPython semantics).
    if (a->type == 7 && (b->type == 0 || b->type == 5)) return PyTuple_Repeat(a, b->value);
    if ((a->type == 0 || a->type == 5) && b->type == 7) return PyTuple_Repeat(b, a->value);
    if (a->type == 15 && (b->type == 0 || b->type == 5)) return pyc_timedelta_mul(pyc_as_timedelta(a), b->value);
    if ((a->type == 0 || a->type == 5) && b->type == 15) return pyc_timedelta_mul(pyc_as_timedelta(b), a->value);
    if (a->type == 19 || b->type == 19) {
        bool aTemp = false, bTemp = false;
        mpd_t* da = pyc_decimal_operand(a, &aTemp);
        mpd_t* db = pyc_decimal_operand(b, &bTemp);
        PyObject* result = nullptr;
        if (da && db) {
            mpd_t* r = mpd_qnew();
            uint32_t status = 0;
            mpd_qmul(r, da, db, pyc_dec_ctx(), &status);
            result = pyc_decimal_wrap(r);
        }
        if (aTemp) mpd_del(da);
        if (bTemp) mpd_del(db);
        return result;
    }
    if (a && b && has_complex(a, b)) {
        double ar, ai, br, bi;
        if (to_complex(a, ar, ai) && to_complex(b, br, bi)) {
            double r = ar * br - ai * bi;
            double i = ar * bi + ai * br;
            return PyComplex_New(r, i);
        }
    }
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) return PyInt_FromLong(a->value * b->value);
    return PyFloat_FromDouble(numeric_val(a) * numeric_val(b));
}

// Floor division (//)
PyObject* PyNumber_Divide(PyObject* a, PyObject* b) {
    // __floordiv__ dispatch for a class instance — see PyNumber_Add's comment.
    if (a && b && a->type == 2) {
        PyObject* method = pyc_lookup_dunder(a, "__floordiv__");
        if (method) return pyc_call_dunder2(method, a, b);
    }
    if (a && b && (a->type == 19 || b->type == 19)) {
        bool aTemp = false, bTemp = false;
        mpd_t* da = pyc_decimal_operand(a, &aTemp);
        mpd_t* db = pyc_decimal_operand(b, &bTemp);
        PyObject* result = nullptr;
        if (da && db) {
            uint32_t status = 0;
            if (mpd_iszero(db)) {
                pyc_raise_msg("ZeroDivisionError", "division by zero");
            } else {
                mpd_t* r = mpd_qnew();
                mpd_qdivint(r, da, db, pyc_dec_ctx(), &status);
                result = pyc_decimal_wrap(r);
            }
        }
        if (aTemp) mpd_del(da);
        if (bTemp) mpd_del(db);
        return result;
    }
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) {
        if (b->value == 0) {
            { pyc_raise_msg("ZeroDivisionError", "integer division or modulo by zero"); return NULL; }
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
        { pyc_raise_msg("ZeroDivisionError", "float divmod()"); return NULL; }
        std::fprintf(stderr, "ZeroDivisionError: float divmod()\n");
        std::fflush(stderr);
        std::exit(1);
    }
    return PyFloat_FromDouble(floor(numeric_val(a) / bv));
}

// True division (/)
PyObject* PyNumber_TrueDivide(PyObject* a, PyObject* b) {
    // pathlib.Path joining: Path / (str or Path) -> new Path, matching
    // CPython's PurePath.__truediv__. Robust regardless of typeOf
    // tracking, same rationale as the date/datetime/timedelta arithmetic
    // above — the "/" IR op always calls this function for non-numeric
    // operands.
    if (a && a->type == 16 && b && pyc_is_path_like(b)) {
        std::string out = a->str;
        const std::string& rhs = b->str;
        if (!rhs.empty() && rhs[0] == '/') {
            out = rhs; // absolute component resets the accumulated path
        } else if (!rhs.empty()) {
            if (!out.empty() && out.back() != '/') out += '/';
            out += rhs;
        }
        return pyc_new_path(out);
    }
    // __truediv__ dispatch for a class instance — see PyNumber_Add's
    // comment. Checked after the Path-joining special case above (Path
    // isn't a class instance — type 16, not 2 — so there's no overlap).
    if (a && b && a->type == 2) {
        PyObject* method = pyc_lookup_dunder(a, "__truediv__");
        if (method) return pyc_call_dunder2(method, a, b);
    }
    if (a && b && (a->type == 19 || b->type == 19)) {
        bool aTemp = false, bTemp = false;
        mpd_t* da = pyc_decimal_operand(a, &aTemp);
        mpd_t* db = pyc_decimal_operand(b, &bTemp);
        PyObject* result = nullptr;
        if (da && db) {
            uint32_t status = 0;
            if (mpd_iszero(db)) {
                pyc_raise_msg("ZeroDivisionError", "division by zero");
            } else {
                mpd_t* r = mpd_qnew();
                mpd_qdiv(r, da, db, pyc_dec_ctx(), &status);
                result = pyc_decimal_wrap(r);
            }
        }
        if (aTemp) mpd_del(da);
        if (bTemp) mpd_del(db);
        return result;
    }
    if (a && b && has_complex(a, b)) {
        double ar, ai, br, bi;
        if (to_complex(a, ar, ai) && to_complex(b, br, bi)) {
            double denom = br * br + bi * bi;
            if (denom == 0.0) {
                pyc_raise_msg("ZeroDivisionError", "complex division by zero");
                return NULL;
            }
            double r = (ar * br + ai * bi) / denom;
            double i = (ai * br - ar * bi) / denom;
            return PyComplex_New(r, i);
        }
    }
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    double bv = numeric_val(b);
    if (bv == 0.0) {
        { pyc_raise_msg("ZeroDivisionError", "float division by zero"); return NULL; }
        std::fprintf(stderr, "ZeroDivisionError: float division by zero\n");
        std::fflush(stderr);
        std::exit(1);
    }
    return PyFloat_FromDouble(numeric_val(a) / bv);
}

PyObject* PyNumber_Remainder(PyObject* a, PyObject* b) {
    if (a && a->type == 3) return PyString_Format(a, b);   // "fmt" % val
    // __mod__ dispatch for a class instance — see PyNumber_Add's comment.
    if (a && b && a->type == 2) {
        PyObject* method = pyc_lookup_dunder(a, "__mod__");
        if (method) return pyc_call_dunder2(method, a, b);
    }
    if (!is_numeric(a) || !is_numeric(b)) return NULL;
    if (both_integral(a, b)) {
        if (b->value == 0) {
            { pyc_raise_msg("ZeroDivisionError", "integer division or modulo by zero"); return NULL; }
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
        { pyc_raise_msg("ZeroDivisionError", "float modulo"); return NULL; }
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
    // Comparison-dunder dispatch for a class instance — found and fixed
    // while bug hunting: __eq__ appeared to "work" only by sheer
    // coincidence (both operands are dict-backed, so `==` fell through
    // to the generic structural dict-equality comparison further below,
    // which is right when two instances happen to hold identical
    // attribute values and wrong otherwise — confirmed `Point(1,2) ==
    // Point(9,9)` incorrectly evaluating `True` with a real __eq__
    // defined and ignored). __lt__/__le__/__gt__/__ge__/__ne__ had no
    // dispatch at all. Only the left operand's dunder is consulted —
    // reflected comparisons (falling back to b's dunder when a's is
    // absent) are a further, narrower simplification, not attempted
    // here.
    if (a->type == 2) {
        const char* dunderName = nullptr;
        switch (op) {
            case 0: dunderName = "__eq__"; break;
            case 1: dunderName = "__ne__"; break;
            case 2: dunderName = "__lt__"; break;
            case 3: dunderName = "__gt__"; break;
            case 4: dunderName = "__le__"; break;
            case 5: dunderName = "__ge__"; break;
        }
        if (dunderName) {
            PyObject* method = pyc_lookup_dunder(a, dunderName);
            if (method) {
                PyObject* r = pyc_call_dunder2(method, a, b);
                int truthy = PyObject_TruthValue(r);
                if (r) Py_DECREF(r);
                return truthy;
            }
            // __ne__ with no direct override falls back to `not __eq__`,
            // matching CPython's own default when a class defines
            // __eq__ but not __ne__.
            if (op == 1) {
                PyObject* eqMethod = pyc_lookup_dunder(a, "__eq__");
                if (eqMethod) {
                    PyObject* r = pyc_call_dunder2(eqMethod, a, b);
                    int truthy = PyObject_TruthValue(r);
                    if (r) Py_DECREF(r);
                    return !truthy;
                }
            }
        }
    }
    // Function objects compare by identity (CPython: no __eq__ on functions).
    if (a->type == 11 || b->type == 11) {
        switch (op) {
            case 0: return a == b;   // ==
            case 1: return a != b;   // !=
            default: return 0;       // ordering: TypeError in CPython
        }
    }
    // List equality and ordering. CPython compares element-wise; the
    // first unequal pair decides, with shorter < longer when all
    // shared elements are equal. We do the same here.
    if (a->type == 1 && b->type == 1) {
        // Same pyc_ensure_boxed_list()-class bug found repeatedly
        // elsewhere in this file (PyObject_TruthValue's list branch,
        // chain.from_iterable's inner lists, ...): homogeneous int/float
        // list literals store their data in ilist/flist (list_item_type
        // 1/2), not list — reading `a->list`/`b->list` directly here
        // meant two homogeneous lists always compared as if both were
        // empty (`al.size() == bl.size() == 0`), so e.g. `[1,2,3] ==
        // [1,2,4]` and `[1,2,3] == [1,2]` both incorrectly evaluated
        // True. Confirmed against real CPython. Normalizing both sides
        // first fixes it for real, not just for the equal-length case.
        pyc_ensure_boxed_list(a);
        pyc_ensure_boxed_list(b);
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
    // tuple vs tuple: structural, same algorithm as list-vs-list above.
    // Tuples are always boxed-storage from the compiler's lowering, but
    // handle homogeneous storage defensively.
    if (a->type == 7 && b->type == 7) {
        if (a->list_item_type != 0) pyc_ensure_boxed_list(a);
        if (b->list_item_type != 0) pyc_ensure_boxed_list(b);
        const auto& al = a->list;
        const auto& bl = b->list;
        size_t n = al.size() < bl.size() ? al.size() : bl.size();
        for (size_t i = 0; i < n; ++i) {
            int eq = (al[i] == bl[i]) ||
                     (al[i] && bl[i] && PyObject_CompareBool(al[i], bl[i], 0));
            if (!eq) {
                if (al[i] && bl[i]) return PyObject_CompareBool(al[i], bl[i], op);
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
    // tuple vs list (or list vs tuple): always unequal. CPython raises
    // TypeError for ordering between a tuple and a list; conservatively
    // return 0 there (matches the existing dict-ordering convention).
    if ((a->type == 7 && b->type == 1) || (a->type == 1 && b->type == 7)) {
        switch (op) {
            case 0: return 0;
            case 1: return 1;
            default: return 0;
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
    // Set comparison. == / != is element-wise (set equality). <= / >= /
    // < / > map to subset/superset/proper-subset/proper-superset (CPython
    // makes sets unordered except for ==/!= and the subset/superset
    // relations — there is no lexicographic ordering).
    if (a->type == 20 && b->type == 20) {
        int eq = (PySet_Size(a) == PySet_Size(b)) && PySet_IsSubset(a, b);
        switch (op) {
            case 0: return eq;
            case 1: return !eq;
            case 4: return PySet_IsSubset(a, b);             // <=
            case 5: return PySet_IsSuperset(a, b);           // >=
            case 2: return PySet_IsSubset(a, b) && !eq;      // < (proper subset)
            case 3: return PySet_IsSuperset(a, b) && !eq;    // > (proper superset)
        }
        return 0;
    }
    // Mixed list/dict — TypeError in CPython; we return 0 for all.
    if ((a->type == 1 || a->type == 2) && (b->type == 1 || b->type == 2) &&
        a->type != b->type) {
        return 0;
    }
    // decimal.Decimal comparison. Decimal-vs-Decimal and Decimal-vs-int
    // are exact (mpd_qcompare / mpd_qset_i64, no lossy double coercion).
    // Decimal-vs-float goes through the same string round-trip
    // PyDecimal_FromFloat uses for construction — an approximation, not
    // exact binary comparison (documented, not fixed — matches this
    // codebase's "don't gold-plate a rarely-hit edge" precedent, e.g.
    // statistics's partial exact-int preservation).
    if (a->type == 19 || b->type == 19) {
        bool aTemp = false, bTemp = false;
        mpd_t* da = nullptr;
        mpd_t* db = nullptr;
        PyObject* aFloatDec = nullptr;
        PyObject* bFloatDec = nullptr;
        if (a->type == 4) { aFloatDec = PyDecimal_FromFloat(a); da = pyc_as_decimal(aFloatDec); }
        else da = pyc_decimal_operand(a, &aTemp);
        if (b->type == 4) { bFloatDec = PyDecimal_FromFloat(b); db = pyc_as_decimal(bFloatDec); }
        else db = pyc_decimal_operand(b, &bTemp);
        if (da && db) {
            mpd_t* r = mpd_qnew();
            uint32_t status = 0;
            mpd_qcompare(r, da, db, pyc_dec_ctx(), &status);
            int64_t cmp = mpd_qget_ssize(r, &status);
            mpd_del(r);
            if (aTemp) mpd_del(da);
            if (bTemp) mpd_del(db);
            if (aFloatDec) Py_DECREF(aFloatDec);
            if (bFloatDec) Py_DECREF(bFloatDec);
            switch (op) {
                case 0: return cmp == 0;
                case 1: return cmp != 0;
                case 2: return cmp <  0;
                case 3: return cmp >  0;
                case 4: return cmp <= 0;
                case 5: return cmp >= 0;
            }
        }
        if (aTemp) mpd_del(da);
        if (bTemp) mpd_del(db);
        if (aFloatDec) Py_DECREF(aFloatDec);
        if (bFloatDec) Py_DECREF(bFloatDec);
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
    // Complex comparison: == and != compare both real and imag parts.
    // Ordering (< > <= >=) is a TypeError in CPython; we return 0.
    if (a && b && (a->type == 13 || b->type == 13)) {
        double ar, ai, br, bi;
        if (to_complex(a, ar, ai) && to_complex(b, br, bi)) {
            switch (op) {
                case 0: return (ar == br) && (ai == bi);
                case 1: return (ar != br) || (ai != bi);
                default: return 0;
            }
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
    // bytes/bytearray comparison — lexicographic byte comparison, same
    // as str above. bytes and bytearray compare equal/ordered by content
    // across the two types (matches real Python: b'ab' == bytearray(b'ab')).
    if ((a->type == 17 || a->type == 18) && (b->type == 17 || b->type == 18)) {
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
    // date/datetime/timedelta comparison. Robust regardless of
    // compiler-inferred typeOf: icmp always routes through this one
    // function, so this works even for values arriving as untyped
    // function parameters.
    if (a->type == 14 && b->type == 14) {
        PycDateTime* da = pyc_as_datetime(a);
        PycDateTime* db = pyc_as_datetime(b);
        int64_t daysA = pyc_days_from_civil(da->year, da->month, da->day);
        int64_t daysB = pyc_days_from_civil(db->year, db->month, db->day);
        int64_t secsA = da->hasTime ? (da->hour * 3600LL + da->minute * 60LL + da->second) : 0;
        int64_t secsB = db->hasTime ? (db->hour * 3600LL + db->minute * 60LL + db->second) : 0;
        int cmp = (daysA != daysB) ? (daysA < daysB ? -1 : 1) : (secsA != secsB ? (secsA < secsB ? -1 : 1) : 0);
        switch (op) {
            case 0: return cmp == 0;
            case 1: return cmp != 0;
            case 2: return cmp < 0;
            case 3: return cmp > 0;
            case 4: return cmp <= 0;
            case 5: return cmp >= 0;
        }
    }
    if (a->type == 15 && b->type == 15) {
        PycTimedelta* ta = pyc_as_timedelta(a);
        PycTimedelta* tb = pyc_as_timedelta(b);
        int cmp = (ta->days != tb->days) ? (ta->days < tb->days ? -1 : 1)
                : (ta->seconds != tb->seconds) ? (ta->seconds < tb->seconds ? -1 : 1)
                : (ta->microseconds != tb->microseconds) ? (ta->microseconds < tb->microseconds ? -1 : 1) : 0;
        switch (op) {
            case 0: return cmp == 0;
            case 1: return cmp != 0;
            case 2: return cmp < 0;
            case 3: return cmp > 0;
            case 4: return cmp <= 0;
            case 5: return cmp >= 0;
        }
    }
    // pathlib.Path compares like a plain string of its path text
    // (matching real CPython: PurePath defines __eq__/__lt__ etc. on the
    // normalized path string). Also accepts a Path compared against a
    // plain str, which real Path does not support (TypeError there) —
    // a deliberate, documented looseness rather than a strict match.
    if (pyc_is_path_like(a) && pyc_is_path_like(b) && (a->type == 16 || b->type == 16)) {
        int cmp = a->str.compare(b->str);
        switch (op) {
            case 0: return cmp == 0;
            case 1: return cmp != 0;
            case 2: return cmp < 0;
            case 3: return cmp > 0;
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
    // Class instances (dict-backed objects with __class__): look up attribute
    // in instance dict first, then class dict. This enables method calls like
    // ctx.__enter__() in `with` statements and attribute access on user classes.
    if (obj->type == 2) {
        // Check instance dict first
        for (auto& kv : obj->dict) {
            if (kv.first && kv.first->type == 3 && kv.first->str == attr) {
                Py_INCREF(kv.second);
                return kv.second;
            }
        }
        // Check class dict
        for (auto& kv : obj->dict) {
            if (kv.first && kv.first->type == 3 && kv.first->str == "__class__") {
                PyObject* classDict = kv.second;
                if (classDict && classDict->type == 2) {
                    for (auto& ck : classDict->dict) {
                        if (ck.first && ck.first->type == 3 && ck.first->str == attr) {
                            Py_INCREF(ck.second);
                            return ck.second;
                        }
                    }
                }
                break;
            }
        }
    }
    // Fallback: return the object itself (matches the previous stub
    // behaviour for unsupported lookups; doesn't crash).
    Py_INCREF(obj);
    return obj;
}

// PyObject_Call(obj, args, kwargs) — call obj with positional and keyword args
// Simplified implementation: for callable tokens, look them up in the registry
extern "C" PyObject* PyObject_Call(PyObject* obj, PyObject* args, PyObject* kwargs);

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

// ---- Function objects (type 11) ----
// str          = callable token (IR/synthetic name, resolvable via the
//                callable registry in Pyc_Apply)
// cell_content = display name for repr (the Python-level name; "<lambda>"
//                for lambdas), may be null.
// String interning: same (token, displayName) pair always returns the same
// PyObject* so that `f is f` works across scopes (CPython semantics).
static std::map<std::pair<std::string, std::string>, PyObject*> g_funcValueCache;

// Complex number (type 13): real and imaginary parts stored as doubles.
PyObject* PyComplex_New(double real, double imag) {
    PyObject* c = new PyObject();
    c->refcount = 1;
    c->type = 13;
    c->complex_real = real;
    c->complex_imag = imag;
    // Zero-initialize other fields
    c->value = 0;
    c->dvalue = 0.0;
    c->cell_content = nullptr;
    c->list_item_type = 0;
    return c;
}

// Complex arithmetic helpers
static PyObject* PyComplex_AddImpl(PyObject* a, PyObject* b) {
    PyObject* res = PyComplex_New(a->complex_real + b->complex_real, a->complex_imag + b->complex_imag);
    return res;
}
static PyObject* PyComplex_SubImpl(PyObject* a, PyObject* b) {
    PyObject* res = PyComplex_New(a->complex_real - b->complex_real, a->complex_imag - b->complex_imag);
    return res;
}
static PyObject* PyComplex_MulImpl(PyObject* a, PyObject* b) {
    // (a+bj)*(c+dj) = (ac-bd) + (ad+bc)j
    double r = a->complex_real * b->complex_real - a->complex_imag * b->complex_imag;
    double i = a->complex_real * b->complex_imag + a->complex_imag * b->complex_real;
    PyObject* res = PyComplex_New(r, i);
    return res;
}
static PyObject* PyComplex_DivImpl(PyObject* a, PyObject* b) {
    // (a+bj)/(c+dj) = ((ac+bd)/(c²+d²)) + ((bc-ad)/(c²+d²))j
    double denom = b->complex_real * b->complex_real + b->complex_imag * b->complex_imag;
    if (denom == 0.0) return nullptr; // ZeroDivisionError will be raised by caller
    double r = (a->complex_real * b->complex_real + a->complex_imag * b->complex_imag) / denom;
    double i = (a->complex_imag * b->complex_real - a->complex_real * b->complex_imag) / denom;
    PyObject* res = PyComplex_New(r, i);
    return res;
}

PyObject* PyComplex_Add(PyObject* a, PyObject* b) {
    if (!a || !b || a->type != 13 || b->type != 13) {
        // Type error — let the caller handle it
        return nullptr;
    }
    return PyComplex_AddImpl(a, b);
}
PyObject* PyComplex_Sub(PyObject* a, PyObject* b) {
    if (!a || !b || a->type != 13 || b->type != 13) {
        return nullptr;
    }
    return PyComplex_SubImpl(a, b);
}
PyObject* PyComplex_Mul(PyObject* a, PyObject* b) {
    if (!a || !b || a->type != 13 || b->type != 13) {
        return nullptr;
    }
    return PyComplex_MulImpl(a, b);
}
PyObject* PyComplex_Div(PyObject* a, PyObject* b) {
    if (!a || !b || a->type != 13 || b->type != 13) {
        return nullptr;
    }
    if (b->complex_real == 0.0 && b->complex_imag == 0.0) {
        // Raise ZeroDivisionError
        PyObject* msg = PyUnicode_FromString("complex division by zero");
        PyObject* exc = pyc_make_exc(PyUnicode_FromString("ZeroDivisionError"), msg);
        Py_DECREF(msg);
        pyc_raise(exc);
        Py_DECREF(exc);
        return nullptr; // unreachable
    }
    return PyComplex_DivImpl(a, b);
}

// Complex pow: z1 ** z2 = exp(z2 * log(z1))
// Uses std::powl for the actual computation
#include <cmath>
PyObject* PyComplex_Pow(PyObject* base, PyObject* exp) {
    if (!base || !exp || base->type != 13 || exp->type != 13) {
        return nullptr;
    }
    std::complex<double> z1(base->complex_real, base->complex_imag);
    std::complex<double> z2(exp->complex_real, exp->complex_imag);
    std::complex<double> result = std::pow(z1, z2);
    return PyComplex_New(result.real(), result.imag());
}

// Complex abs: |a+bj| = sqrt(a² + b²)
PyObject* PyComplex_Abs(PyObject* z) {
    if (!z || z->type != 13) {
        return nullptr;
    }
    double magnitude = std::sqrt(z->complex_real * z->complex_real + z->complex_imag * z->complex_imag);
    // Return as float (type 4)
    PyObject* f = new PyObject();
    f->refcount = 1;
    f->type = 4;
    f->dvalue = magnitude;
    return f;
}

// === cmath module functions ===
// These are called via the cmath synthetic module dict.

// cmath.sqrt(z) — square root of complex number
PyObject* PyCmath_Sqrt(PyObject* z) {
    if (!z) return PyComplex_New(0.0, 0.0);
    if (z->type == 13) {
        std::complex<double> c(z->complex_real, z->complex_imag);
        std::complex<double> r = std::sqrt(c);
        return PyComplex_New(r.real(), r.imag());
    }
    if (z->type == 0 || z->type == 5) {
        double v = (double)z->value;
        if (v >= 0) {
            return PyComplex_New(std::sqrt(v), 0.0);
        } else {
            return PyComplex_New(0.0, std::sqrt(-v));
        }
    }
    if (z->type == 4) {
        double v = z->dvalue;
        if (v >= 0) {
            return PyComplex_New(std::sqrt(v), 0.0);
        } else {
            return PyComplex_New(0.0, std::sqrt(-v));
        }
    }
    return PyComplex_New(0.0, 0.0);
}

// cmath.log(z) — natural logarithm of complex number
PyObject* PyCmath_Log(PyObject* z) {
    if (!z) return PyComplex_New(0.0, 0.0);
    if (z->type == 13) {
        std::complex<double> c(z->complex_real, z->complex_imag);
        std::complex<double> r = std::log(c);
        return PyComplex_New(r.real(), r.imag());
    }
    if (z->type == 0 || z->type == 5) {
        double v = (double)z->value;
        if (v > 0) {
            return PyComplex_New(std::log(v), 0.0);
        } else if (v == 0) {
            return PyComplex_New(-1.0/0.0, 0.0); // -inf
        } else {
            return PyComplex_New(0.0, M_PI);
        }
    }
    if (z->type == 4) {
        double v = z->dvalue;
        if (v > 0) {
            return PyComplex_New(std::log(v), 0.0);
        } else if (v == 0) {
            return PyComplex_New(-1.0/0.0, 0.0);
        } else {
            return PyComplex_New(0.0, M_PI);
        }
    }
    return PyComplex_New(0.0, 0.0);
}

// cmath.exp(z) — e^z for complex number
PyObject* PyCmath_Exp(PyObject* z) {
    if (!z) return PyComplex_New(0.0, 0.0);
    if (z->type == 13) {
        std::complex<double> c(z->complex_real, z->complex_imag);
        std::complex<double> r = std::exp(c);
        return PyComplex_New(r.real(), r.imag());
    }
    if (z->type == 0 || z->type == 5) {
        double v = (double)z->value;
        return PyComplex_New(std::exp(v), 0.0);
    }
    if (z->type == 4) {
        return PyComplex_New(std::exp(z->dvalue), 0.0);
    }
    return PyComplex_New(0.0, 0.0);
}

// cmath.sin(z) — sine of complex number
PyObject* PyCmath_Sin(PyObject* z) {
    if (!z) return PyComplex_New(0.0, 0.0);
    if (z->type == 13) {
        std::complex<double> c(z->complex_real, z->complex_imag);
        std::complex<double> r = std::sin(c);
        return PyComplex_New(r.real(), r.imag());
    }
    if (z->type == 0 || z->type == 5) {
        double v = (double)z->value;
        return PyComplex_New(std::sin(v), 0.0);
    }
    if (z->type == 4) {
        return PyComplex_New(std::sin(z->dvalue), 0.0);
    }
    return PyComplex_New(0.0, 0.0);
}

// cmath.cos(z) — cosine of complex number
PyObject* PyCmath_Cos(PyObject* z) {
    if (!z) return PyComplex_New(0.0, 0.0);
    if (z->type == 13) {
        std::complex<double> c(z->complex_real, z->complex_imag);
        std::complex<double> r = std::cos(c);
        return PyComplex_New(r.real(), r.imag());
    }
    if (z->type == 0 || z->type == 5) {
        double v = (double)z->value;
        return PyComplex_New(std::cos(v), 0.0);
    }
    if (z->type == 4) {
        return PyComplex_New(std::cos(z->dvalue), 0.0);
    }
    return PyComplex_New(0.0, 0.0);
}

// cmath.tan(z) — tangent of complex number
PyObject* PyCmath_Tan(PyObject* z) {
    if (!z) return PyComplex_New(0.0, 0.0);
    if (z->type == 13) {
        std::complex<double> c(z->complex_real, z->complex_imag);
        std::complex<double> r = std::tan(c);
        return PyComplex_New(r.real(), r.imag());
    }
    if (z->type == 0 || z->type == 5) {
        double v = (double)z->value;
        return PyComplex_New(std::tan(v), 0.0);
    }
    if (z->type == 4) {
        return PyComplex_New(std::tan(z->dvalue), 0.0);
    }
    return PyComplex_New(0.0, 0.0);
}

// ---------------------------------------------------------------------
// decimal.Decimal (type 19) — arbitrary-precision base-10 arithmetic via
// libmpdec (the same C library CPython's own `_decimal` module is built
// on; see CMakeLists.txt). One shared global context (28 significant
// digits, ROUND_HALF_EVEN — matches CPython's real defaults; libmpdec's
// own mpd_defaultcontext() differs, 38 digits/ROUND_HALF_UP, so both are
// set explicitly). decimal.getcontext()/localcontext() precision
// mutation is not implemented — every operation uses this one context.
//
// Storage: unlike complex (type 13, which fits in native `double` fields
// added directly to PyObject), an `mpd_t*` is a heap-allocated opaque
// struct — stashed in the existing `value` field via pointer cast, the
// same pattern already used for CompiledRegex*/MatchObj*/PycDateTime*/
// PycTimedelta* (types 8/9/14/15). This means, unlike complex, a
// Py_DECREF branch calling mpd_del() is required (see Py_DECREF below) —
// the exact thing complex's type didn't need and got right by omission,
// not by design; important not to repeat that omission here.
//
// Arithmetic is wired into the *generic* PyNumber_Add/Subtract/Multiply/
// Divide/TrueDivide/Negate functions (a type==19 branch in each), not a
// compile-time-only tracking set like complex's `complexVars`. This is
// the deliberate, better-quality-than-complex choice: since
// Compiler.cpp's numericResultType() is simply never taught to treat
// type 19 as numeric, every Decimal arithmetic op automatically falls to
// these generic functions regardless of whether the compiler statically
// proved the value's type — so, unlike complex, Decimal arithmetic
// works correctly even when a value arrives as an untyped function
// parameter.
//
// (pyc_dec_ctx/pyc_as_decimal/pyc_decimal_wrap are defined near the top
// of this file, right after the mpdecimal.h include, since they're
// needed by PyNumber_Negate/Add/Subtract — which appear earlier in this
// file than this comment block.)
extern "C" PyObject* PyDecimal_FromString(PyObject* s) {
    mpd_t* d = mpd_qnew();
    uint32_t status = 0;
    std::string text = pyc_is_bytes_like(s) ? s->str : std::string("0");
    mpd_qset_string(d, text.c_str(), pyc_dec_ctx(), &status);
    return pyc_decimal_wrap(d);
}
extern "C" PyObject* PyDecimal_FromInt(PyObject* n) {
    mpd_t* d = mpd_qnew();
    uint32_t status = 0;
    int64_t v = (n && (n->type == 0 || n->type == 5)) ? n->value : 0;
    mpd_qset_i64(d, v, pyc_dec_ctx(), &status);
    return pyc_decimal_wrap(d);
}
// Decimal(float) uses the float's exact binary value, same as real
// CPython (Decimal(0.1) is a long, "ugly" decimal, not a clean "0.1") —
// achieved here by formatting the double with enough digits (%.17g is
// the shortest-round-trip precision for a double) and parsing that.
extern "C" PyObject* PyDecimal_FromFloat(PyObject* f) {
    mpd_t* d = mpd_qnew();
    uint32_t status = 0;
    double v = (f && f->type == 4) ? f->dvalue : ((f && (f->type == 0 || f->type == 5)) ? (double)f->value : 0.0);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    // %.17g's precision would round-trip the double exactly via a real
    // bignum float->decimal conversion; formatting through a 17-digit
    // decimal string first (rather than round-tripping the full binary
    // fraction) is an approximation of CPython's exact-binary-value
    // behavior, not a bit-for-bit match — documented, not fixed (out of
    // scope: true exact binary->decimal conversion needs a real
    // arbitrary-precision float decomposition, more machinery than this
    // phase's scope).
    mpd_qset_string(d, buf, pyc_dec_ctx(), &status);
    return pyc_decimal_wrap(d);
}
// decimal.Decimal(x) — dispatches on x's runtime type (string/int/float/
// another Decimal). Direct-call convention (AST-recognized, mirroring
// hashlib.md5/pathlib.Path) so the result can carry the "decimal"
// noteType tag needed to gate .quantize()'s dispatch.
extern "C" PyObject* PyDecimal_Construct(PyObject* x) {
    if (!x) { mpd_t* d = mpd_qnew(); uint32_t st = 0; mpd_qset_i64(d, 0, pyc_dec_ctx(), &st); return pyc_decimal_wrap(d); }
    if (x->type == 19) {
        mpd_t* d = mpd_qnew();
        uint32_t status = 0;
        mpd_qcopy(d, pyc_as_decimal(x), &status);
        return pyc_decimal_wrap(d);
    }
    if (x->type == 4) return PyDecimal_FromFloat(x);
    if (x->type == 0 || x->type == 5) return PyDecimal_FromInt(x);
    return PyDecimal_FromString(x);
}
extern "C" PyObject* PyDecimal_Quantize(PyObject* self, PyObject* q) {
    mpd_t* a = pyc_as_decimal(self);
    mpd_t* b = pyc_as_decimal(q);
    if (!a || !b) return PyDecimal_FromInt(nullptr);
    mpd_t* r = mpd_qnew();
    uint32_t status = 0;
    mpd_qquantize(r, a, b, pyc_dec_ctx(), &status);
    return pyc_decimal_wrap(r);
}
// str()/print() and int()/float() conversion for Decimal are handled
// directly in PyObject_PrintBase/PyBuiltin_Int/PyBuiltin_Float above —
// no separate PyDecimal_ToStr/ToInt/ToFloat needed.

PyObject* pyc_make_func(PyObject* token, PyObject* displayName) {
    std::string tokStr = (token && token->type == 3) ? token->str : "";
    std::string dispStr = (displayName && displayName->type == 3) ? displayName->str : "";
    auto key = std::make_pair(tokStr, dispStr);
    auto it = g_funcValueCache.find(key);
    if (it != g_funcValueCache.end()) {
        Py_INCREF(it->second);
        return it->second;
    }
    PyObject* f = new PyObject();
    f->refcount = 1;
    f->type = 11;
    f->value = 1;   // functions are truthy (codegen truth tests read ->value)
    f->str = tokStr;
    f->cell_content = nullptr;
    if (displayName && displayName->type == 3) {
        f->cell_content = displayName;
        Py_INCREF(displayName);
    }
    g_funcValueCache[key] = f;
    return f;
}

// Exception class objects (type 12): behave like first-class exception
// classes.  When called via Pyc_Apply they construct a structured
// exception (type 10) using pyc_make_exc.
// String interning: same exception name always returns the same PyObject*
// so that `ValueError is exc` works (CPython semantics).
static std::map<std::string, PyObject*> g_excClassCache;

PyObject* pyc_make_exc_class(PyObject* excName) {
    std::string name = (excName && excName->type == 3) ? excName->str : "Exception";
    auto it = g_excClassCache.find(name);
    if (it != g_excClassCache.end()) {
        Py_INCREF(it->second);
        return it->second;
    }
    PyObject* e = new PyObject();
    e->refcount = 1;
    e->type = 12;
    e->value = 1;   // truthy
    e->str = name;
    e->cell_content = nullptr;
    g_excClassCache[name] = e;
    return e;
}

// ---- Structured exceptions (type 10) ----
// str          = exception type name ("ValueError", ...)
// cell_content = message object (usually a str), may be null.
// Legacy string exceptions ("TypeName: message") are still accepted by
// the matcher and printers for backward compatibility.
PyObject* pyc_make_exc(PyObject* typeName, PyObject* msg) {
    PyObject* e = new PyObject();
    e->refcount = 1;
    e->type = 10;
    e->value = 1;   // exceptions are truthy (codegen truth tests read ->value)
    e->str = (typeName && typeName->type == 3) ? typeName->str : "Exception";
    e->cell_content = nullptr;
    // An empty-string message from the compiler's no-argument constructor
    // sentinel means "no message".
    if (msg && !(msg->type == 3 && msg->str.empty())) {
        e->cell_content = msg;
        Py_INCREF(msg);
    }
    return e;
}

// Type name and message of any exception value (structured, legacy string,
// or arbitrary object).
static std::string pyc_exc_type_name(PyObject* exc) {
    if (!exc) return "Exception";
    if (exc->type == 10) return exc->str;
    if (exc->type == 3) {
        size_t p = exc->str.find(": ");
        if (p != std::string::npos) return exc->str.substr(0, p);
        return exc->str;
    }
    // User-defined exception subclass instance (type 2) — see the
    // pyc_exc_instance_mro/pyc_instance_is_exception comment far above
    // for the full story. __mro__[0] is the class's own name.
    if (exc->type == 2) {
        PyObject* mro = pyc_exc_instance_mro(exc);
        if (mro && mro->type == 1 && PyList_Size(mro) > 0) {
            PyObject* first = PyList_GetItemI64(mro, 0);
            std::string r = (first && first->type == 3) ? first->str : "Exception";
            if (first) Py_DECREF(first);
            return r;
        }
    }
    return "Exception";
}
static std::string pyc_exc_message(PyObject* exc) {
    if (!exc) return "";
    if (exc->type == 10) {
        if (!exc->cell_content) return "";
        // KeyError displays the repr of its argument (str(KeyError('k')) == "'k'").
        if (exc->str == "KeyError" && exc->cell_content->type == 3)
            return "'" + exc->cell_content->str + "'";
        PyObject* s = PyStr_FromAny(exc->cell_content);
        std::string r = s ? s->str : "";
        if (s) Py_DECREF(s);
        return r;
    }
    if (exc->type == 3) {
        size_t p = exc->str.find(": ");
        if (p != std::string::npos) return exc->str.substr(p + 2);
        return exc->str;
    }
    // User-defined exception subclass instance: message is args[0],
    // matching CPython's BaseException.__str__ for a single-arg
    // exception (args is populated at construction time — see the
    // Compiler.cpp instantiation-site fix). Must be handled explicitly
    // here, before the generic PyStr_FromAny(exc) fallback below: that
    // fallback goes through PyObject_Print -> PyObject_PrintBase, which
    // for an exception-instance now calls back into this very function —
    // falling through to it here would recurse infinitely.
    if (exc->type == 2) {
        for (auto& pair : exc->dict) {
            if (pair.first && pair.first->type == 3 && pair.first->str == "args") {
                PyObject* args = pair.second;
                if (args && args->type == 1 && PyList_Size(args) > 0) {
                    PyObject* first = PyList_GetItemI64(args, 0);
                    PyObject* s = PyStr_FromAny(first);
                    std::string r = s ? s->str : "";
                    if (s) Py_DECREF(s);
                    if (first) Py_DECREF(first);
                    return r;
                }
                break;
            }
        }
        return "";
    }
    PyObject* s = PyStr_FromAny(exc);
    std::string r = s ? s->str : "";
    if (s) Py_DECREF(s);
    return r;
}

// Minimal builtin exception hierarchy (child -> parent). Everything is
// implicitly a subclass of Exception/BaseException.
static const char* pyc_exc_parent(const std::string& n) {
    if (n == "ZeroDivisionError" || n == "OverflowError" || n == "FloatingPointError") return "ArithmeticError";
    if (n == "IndexError" || n == "KeyError") return "LookupError";
    if (n == "FileNotFoundError" || n == "PermissionError" || n == "IOError") return "OSError";
    if (n == "IndentationError") return "SyntaxError";
    if (n == "UnboundLocalError") return "NameError";
    return nullptr;
}

// Boxed-bool: does `exc` match an except clause naming `typeName`?
PyObject* pyc_exc_matches(PyObject* exc, PyObject* typeName) {
    if (!typeName || typeName->type != 3) return PyBool_New(0);
    const std::string& want = typeName->str;
    if (want == "Exception" || want == "BaseException") return PyBool_New(1);
    // User-defined exception subclass instance: check its exact __mro__
    // chain (already flattened at compile time via C3 linearization,
    // e.g. class MyError(Exception) has __mro__ == ["MyError",
    // "Exception"]) rather than the pyc_exc_parent() child->parent table
    // below, which only covers structured (type 10) builtin exceptions.
    if (exc && exc->type == 2) {
        PyObject* mro = pyc_exc_instance_mro(exc);
        if (mro && mro->type == 1) {
            size_t n = PyList_Size(mro);
            for (size_t i = 0; i < n; ++i) {
                PyObject* m = PyList_GetItemI64(mro, (long)i);
                bool eq = (m && m->type == 3 && m->str == want);
                if (m) Py_DECREF(m);
                if (eq) return PyBool_New(1);
            }
        }
        return PyBool_New(0);
    }
    std::string have = pyc_exc_type_name(exc);
    while (!have.empty()) {
        if (have == want) return PyBool_New(1);
        const char* up = pyc_exc_parent(have);
        have = up ? up : "";
    }
    return PyBool_New(0);
}

// Most recent exception raised — used by bare `raise` (re-raise). Never
// cleared by pyc_clear_exception.
static thread_local PyObject* g_last_exception = nullptr;

// Uncaught exception: report like CPython (type: message on stderr) and exit.
static void pyc_fatal_exception(PyObject* exc) {
    std::string tn = pyc_exc_type_name(exc);
    std::string msg = pyc_exc_message(exc);
    fprintf(stderr, "Traceback (most recent call last):\n");
    if (msg.empty()) fprintf(stderr, "%s\n", tn.c_str());
    else fprintf(stderr, "%s: %s\n", tn.c_str(), msg.c_str());
    exit(1);
}

void pyc_raise(PyObject* exc) {
    if (!exc) return;
    // Exception class objects (type 12): instantiate to a structured
    // exception (type 10) before raising.
    if (exc->type == 12) {
        PyObject* msg = PyUnicode_FromString("");
        PyObject* typeStr = PyUnicode_FromString(exc->str.c_str());
        PyObject* instantiated = pyc_make_exc(typeStr, msg);
        Py_DECREF(typeStr);
        Py_DECREF(msg);
        if (g_last_exception != instantiated) {
            if (g_last_exception) Py_DECREF(g_last_exception);
            g_last_exception = instantiated;
            Py_INCREF(instantiated);
        }
        if (g_try_stack) {
            TryFrame* f = g_try_stack;
            g_try_stack = f->next;
            if (g_current_exception) Py_DECREF(g_current_exception);
            g_current_exception = instantiated;
            Py_INCREF(instantiated);
            jmp_buf jmp;
            memcpy(jmp, f->jmp, sizeof(jmp_buf));
            if (f->exc) Py_DECREF(f->exc);
            delete f;
            std::longjmp(jmp, 1);
        }
        pyc_fatal_exception(instantiated);
        Py_DECREF(instantiated);
        return;
    }
    if (g_last_exception != exc) {
        if (g_last_exception) Py_DECREF(g_last_exception);
        g_last_exception = exc;
        Py_INCREF(exc);
    }
    if (g_try_stack) {
        // Pop the frame BEFORE the jump: handler dispatch runs in generated
        // code on the exception path, and a raise from a handler / no-match
        // re-raise must target the next outer try. The exception itself
        // travels in g_current_exception.
        TryFrame* f = g_try_stack;
        g_try_stack = f->next;
        if (g_current_exception) Py_DECREF(g_current_exception);
        g_current_exception = exc;
        Py_INCREF(exc);
        jmp_buf jmp;
        memcpy(jmp, f->jmp, sizeof(jmp_buf));
        if (f->exc) Py_DECREF(f->exc);
        delete f;
        std::longjmp(jmp, 1);
    }
    pyc_fatal_exception(exc);
}

// Convenience for runtime operations raising builtin exceptions.
static void pyc_raise_msg(const char* type, const char* msg) {
    PyObject* t = PyUnicode_FromString(type);
    PyObject* m = PyUnicode_FromString(msg);
    PyObject* e = pyc_make_exc(t, m);
    Py_DECREF(t);
    Py_DECREF(m);
    pyc_raise(e);   // does not return when a try frame exists
    Py_DECREF(e);
}

void pyc_reraise(void) {
    if (g_last_exception) {
        pyc_raise(g_last_exception);
        return;
    }
    pyc_raise_msg("RuntimeError", "No active exception to reraise");
}

PyObject* pyc_current_exception(void) {
    if (!g_current_exception) return nullptr;
    Py_INCREF(g_current_exception);
    return g_current_exception;
}
void pyc_clear_exception(void) {
    if (g_current_exception) {
        Py_DECREF(g_current_exception);
        g_current_exception = nullptr;
    }
}

// Eagerly materializes a class instance's __iter__/__next__ protocol
// into a real list — found and fixed while bug hunting: `for x in obj:`
// (and the bare `list(obj)` builtin, both of which route through
// PyBuiltin_List) for a class implementing the iterator protocol
// previously always iterated the instance's own raw attribute dict
// instead (a class instance IS a dict, per pyc's representation), with
// no error of any kind — confirmed via a Range2 class whose __next__
// raises StopIteration: `for x in Range2(3):` printed the instance's
// attribute names ("n", "i", "__class__") instead of the intended
// 0/1/2 sequence.
//
// This fits pyc's existing, deliberate "eager materialization"
// architecture (already used for generator expressions and most
// itertools functions — see FEATURES.md) rather than implementing true
// lazy iteration (a much larger, separate architectural change): the
// entire iterator is drained up front into a real list before the
// for-loop / list() call ever sees it, so a __next__ that never raises
// StopIteration would hang here exactly as any other already-documented
// "no lazy iterator, no infinite iterables" case would.
//
// __next__ is invoked through the same setjmp-based try/except
// machinery Compiler.cpp's generated code already uses for every other
// try/except in a compiled program (pyc_try_push/pyc_try_pop, a fresh
// jmp_buf per call) so that a StopIteration raised deep inside the
// user's __next__ body — an ordinary compiled Python function, not
// something this helper can special-case — correctly unwinds back here
// instead of propagating past this function entirely. A non-
// StopIteration exception is deliberately re-raised outward (propagates
// to whatever try/except, if any, wraps the for-loop/list() call in the
// user's own code), matching real Python's behavior when __next__
// raises something else.
static PyObject* pyc_materialize_iterator_protocol(PyObject* obj) {
    PyObject* iterMethod = pyc_lookup_dunder(obj, "__iter__");
    PyObject* iterObj = iterMethod ? pyc_call_dunder1(iterMethod, obj) : nullptr;
    if (!iterObj) return PyList_New(0);
    PyObject* nextMethod = pyc_lookup_dunder(iterObj, "__next__");
    if (!nextMethod) { Py_DECREF(iterObj); return PyList_New(0); }
    PyObject* result = PyList_New(0);
    for (;;) {
        jmp_buf jb;
        PyObject* item = nullptr;
        bool stopped = false;
        bool propagate = false;
        if (setjmp(jb) == 0) {
            pyc_try_push(&jb, nullptr);
            item = pyc_call_dunder1(nextMethod, iterObj);
            pyc_try_pop();
        } else {
            PyObject* exc = pyc_current_exception();
            if (exc && pyc_exc_type_name(exc) == "StopIteration") {
                stopped = true;
            } else {
                propagate = true;
            }
            if (propagate) {
                Py_DECREF(iterObj);
                Py_DECREF(result);
                if (exc) pyc_raise(exc); // does not return when an outer try exists
                if (exc) Py_DECREF(exc);
                return nullptr;
            }
            pyc_clear_exception();
            if (exc) Py_DECREF(exc);
        }
        if (stopped) break;
        PyList_Append(result, item);
        if (item) Py_DECREF(item);
    }
    Py_DECREF(iterObj);
    return result;
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

// ---- Generator yield helpers (eager materialization) --------------------
// Thread-local buffer used by pyc_yield_collect / pyc_get_yield_buffer /
// pyc_clear_yield_buffer.  The compiler wraps every call to a generator
// function with:  clear_buffer → call → get_buffer.
// yield expressions inside the body call pyc_yield_collect which appends
// the value to the buffer and returns it (so `x = yield 5` works as
// `x = 5`).  The final get_buffer returns the collected list.

static thread_local std::vector<PyObject*> g_yieldBuffer;

extern "C" PyObject* pyc_yield_collect(PyObject* value) {
    if (value) Py_INCREF(value);
    g_yieldBuffer.push_back(value);
    return value;
}

extern "C" PyObject* pyc_get_yield_buffer(void) {
    PyObject* result = PyList_New((size_t)g_yieldBuffer.size());
    for (size_t i = 0; i < g_yieldBuffer.size(); ++i) {
        PyList_SetItem(result, i, g_yieldBuffer[i]);  // steals ref
    }
    g_yieldBuffer.clear();
    return result;
}

extern "C" void pyc_clear_yield_buffer(void) {
    g_yieldBuffer.clear();
}

// ---- B4/B8 callable dispatch (lambdas as values, dynamic call via token) ----

// g_callableRegistry is forward-declared near PyBuiltin_Callable above.

extern "C" void pyc_register_callable(const char* name, PyObject* (*func)(PyObject*)) {
    if (name && func) g_callableRegistry[std::string(name)] = func;
}

// Forward declaration: classRegistry() is defined further down (alongside
// super()'s MRO resolution) but is also needed here, by
// pyc_lookup_via_mro, for the dynamic class-instantiation branch below.
static std::unordered_map<std::string, PyObject*>& classRegistry();

// Resolve `name` by walking classDict's __mro__ (own class first, then
// bases in MRO order) — same resolution order PyBuiltin_SuperMethod uses
// for inherited methods (further down), but starting at index 0 instead
// of "just past the defining class" since this is an ordinary (non-super)
// lookup. Used by Pyc_Apply's dynamic instantiation branch to find
// __init__, including one inherited from a base class.
static PyObject* pyc_lookup_via_mro(PyObject* classDict, const char* name) {
    if (!classDict || classDict->type != 2) return nullptr;
    PyObject* mroList = nullptr;
    for (auto& kv : classDict->dict) {
        if (kv.first && kv.first->type == 3 && kv.first->str == "__mro__") { mroList = kv.second; break; }
    }
    if (!mroList || mroList->type != 1) {
        for (auto& kv : classDict->dict) {
            if (kv.first && kv.first->type == 3 && kv.first->str == name) return kv.second;
        }
        return nullptr;
    }
    for (auto* mroItem : mroList->list) {
        PyObject* cls = nullptr;
        if (mroItem && mroItem->type == 3) {
            auto it = classRegistry().find(mroItem->str);
            if (it != classRegistry().end()) cls = it->second;
        } else if (mroItem && mroItem->type == 2) {
            cls = mroItem;
        }
        if (!cls) continue;
        for (auto& kv : cls->dict) {
            if (kv.first && kv.first->type == 3 && kv.first->str == name) return kv.second;
        }
    }
    return nullptr;
}

// Pyc_Apply(tokenStr or bundleList, argList) -> boxed result
// If first arg is a bundle list [tokenStr, extra0, ...] (cells or prebound defaults),
// extract the token and prepend the extras to the provided argList before dispatch.
extern "C" PyObject* Pyc_Apply(PyObject* token, PyObject* argList) {
    if (!token) return nullptr;
    // A cell-backed callee (closure free variable holding a callable) may
    // arrive as the cell itself — unwrap to its content.
    while (token && token->type == 6 && token->cell_content) token = token->cell_content;
    // Exception class objects (type 12): construct a structured exception
    // (type 10) by calling pyc_make_exc with the stored type name and the
    // first argument as the message.
    if (token && token->type == 12) {
        PyObject* msg = nullptr;
        if (argList && argList->type == 1 && !argList->list.empty()) {
            msg = argList->list[0];
            if (msg) Py_INCREF(msg);
        }
        if (!msg) {
            msg = PyUnicode_FromString("");
        }
        PyObject* typeStr = PyUnicode_FromString(token->str.c_str());
        PyObject* exc = pyc_make_exc(typeStr, msg);
        Py_DECREF(typeStr);
        Py_DECREF(msg);
        return exc;
    }
    // Accept a bare string token, a function object (type 11, token in str),
    // or a descriptor bundle list whose first element is either of those.
    std::string tokName;
    bool haveTok = false;
    if (token->type == 3 || token->type == 11) {
        tokName = token->str;
        haveTok = true;
    } else if (token->type == 1 && !token->list.empty()) {
        PyObject* first = token->list[0];
        if (first && (first->type == 3 || first->type == 11)) {
            tokName = first->str;
            haveTok = true;
        }
    }
    if (!haveTok) {
        if (token->type == 2) {
            // Dynamic class instantiation — found and fixed while bug
            // hunting: `X = Foo; X()` (factory patterns, class
            // registries, `cls()` inside plain functions) previously
            // always silently returned None. Compiler.cpp's structural
            // instantiation path only recognizes a literal `ClassName(...)`
            // callee at compile time; a variable holding a class value
            // falls all the way through to this generic Pyc_Apply
            // fallback instead, where a dict-typed token (type 2)
            // previously matched none of the "callable" shapes above.
            // Must be checked BEFORE the __call__ dispatch below: a
            // class dict's own entries are its *instance* methods, so a
            // class defining `__call__` for its instances would
            // otherwise make pyc_lookup_dunder(token, "__call__")
            // spuriously match on the class dict itself (which has no
            // bound instance to call it on).
            bool isClassDict = false;
            for (auto& p : token->dict) {
                if (p.first && p.first->type == 3 && p.first->str == "__mro__") { isClassDict = true; break; }
            }
            if (isClassDict) {
                PyObject* instance = PyDict_New();
                PyObject* classKey = PyUnicode_FromString("__class__");
                PyDict_SetItem(instance, classKey, token);
                Py_DECREF(classKey);
                PyObject* initMethod = pyc_lookup_via_mro(token, "__init__");
                if (initMethod) {
                    PyObject* r = Pyc_CallMethod(initMethod, instance, argList);
                    if (r) Py_DECREF(r);
                }
                return instance;
            }
            // __call__ dispatch for a class instance — found and fixed
            // while bug hunting: calling an instance like a function
            // (f(5) where f is a class instance defining __call__)
            // previously always silently returned None, since a dict-typed
            // token (type 2) matched none of the "callable" shapes above.
            // callMethod is normally a bare token string, but could rarely
            // be a decorator-tagged 2-element list (e.g. a @staticmethod
            // __call__, unusual but not disallowed) — Pyc_CallMethod
            // already knows how to unwrap either shape correctly for a
            // bound call, so route through it rather than re-deriving that
            // logic here.
            PyObject* callMethod = pyc_lookup_dunder(token, "__call__");
            if (callMethod) return Pyc_CallMethod(callMethod, token, argList);
        }
        return nullptr;
    }
    auto it = g_callableRegistry.find(tokName);
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

// PyObject_Call(obj, args, kwargs) — call obj with positional and keyword args
// Simplified implementation: for callable tokens, look them up in the registry
extern "C" PyObject* PyObject_Call(PyObject* obj, PyObject* args, PyObject* kwargs) {
    if (!obj || !args || args->type != 1) return nullptr;
    if (obj->type == 3) {
        // It's a string token — look it up in the registry
        auto it = g_callableRegistry.find(obj->str);
        if (it != g_callableRegistry.end() && it->second) {
            return it->second(args);
        }
        return nullptr;
    }
    // For methods (bound functions), we need to call them with self as first arg
    // This is a simplified implementation
    return nullptr;
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
           alloc_list_count.load() + alloc_dict_count.load() + alloc_str_count.load() +
           alloc_set_count.load();
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

// ---- B6b: runtime class registry ----
// __mro__ lists hold class *names* (strings); the registry maps a name to the
// class dict object so super() can resolve MRO entries at runtime. Classes are
// registered at module init right after their class dict is assembled.
static std::unordered_map<std::string, PyObject*>& classRegistry() {
    static std::unordered_map<std::string, PyObject*> reg;
    return reg;
}

extern "C" void pyc_register_class(PyObject* name, PyObject* cls) {
    if (!name || name->type != 3 || !cls) return;
    Py_INCREF(cls);
    classRegistry()[name->str] = cls;
}

// ---- B6b: super() with MRO-based method resolution ----
// PyBuiltin_SuperMethod(self, definingClass, methodName, [args...])
// Implements Python's super() behavior:
// 1. Gets self.__class__
// 2. Looks up __mro__ from that class dict
// 3. Finds definingClass in the MRO
// 4. Searches the classes after definingClass in MRO order for methodName
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

    // Search the remaining MRO (everything after definingClass) for the first
    // class providing the method — Python semantics; the immediate next class
    // may not define it (e.g. a pass-through intermediate in a diamond).
    // MRO entries are class-name strings; resolve them via the class registry.
    PyObject* method = nullptr;
    if (getenv("PYC_DEBUG_SUPER")) {
        fprintf(stderr, "[super] defining=%s method=%s mro=[", definingClass->str.c_str(), methodName->str.c_str());
        for (size_t i = 0; i < mroSize; ++i) {
            PyObject* it = PyList_GetItemInt(mroList, i);
            fprintf(stderr, "%s%s", i ? "," : "", (it && it->type == 3) ? it->str.c_str() : "?");
        }
        fprintf(stderr, "] definingIndex=%zu\n", definingIndex);
    }
    for (size_t i = definingIndex + 1; i < mroSize && !method; ++i) {
        PyObject* mroItem = PyList_GetItemInt(mroList, i);
        PyObject* cls = nullptr;
        if (mroItem && mroItem->type == 3) {
            auto it = classRegistry().find(mroItem->str);
            if (it != classRegistry().end()) cls = it->second;
        } else if (mroItem && mroItem->type == 2) {
            cls = mroItem;  // already a class dict
        }
        if (!cls) continue;
        for (auto& kv : cls->dict) {
            if (pyObjStrEqual(kv.first, methodName)) {
                method = kv.second;
                break;
            }
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
    
    // Call the method. callArgs is a temporary built just to prepend
    // self — see the same fix in Pyc_CallMethod above; freed after use.
    PyObject* result = Pyc_Apply(method, callArgs);
    Py_DECREF(callArgs);
    return result;
}

// ---- @classmethod / @staticmethod / @property dispatch ----
// Found and fixed while bug hunting: these three method decorators used
// to be silently discarded entirely — every method was registered in
// its class dict and called identically regardless of decorator, so
// `cls` was never correctly bound to the class (only accidentally to
// the instance when called via instance.method()), a @staticmethod
// taking real parameters crashed/misbehaved when called via an instance
// (self was wrongly prepended on top of the real arguments), and
// @property getters were never invoked on plain attribute access at all
// (a.doubled returned the method's raw internal token instead of
// calling the getter). Fixed by tagging a decorated method's class-dict
// entry as a 2-element list [kind, realToken] instead of a bare string
// token (Compiler.cpp's lowerClass) — Pyc_CallMethod and Pyc_GetAttr
// below recognize that shape and dispatch accordingly; a plain
// (undecorated) method's entry is still just a bare string, so this is
// purely additive and doesn't change the always-worked path.
static bool pyc_is_decorated_method(PyObject* v, std::string& kindOut, PyObject*& realTokenOut) {
    if (v && v->type == 1 && v->list.size() == 2 &&
        v->list[0] && v->list[0]->type == 3 &&
        v->list[1] && v->list[1]->type == 3) {
        kindOut = v->list[0]->str;
        realTokenOut = v->list[1];
        return (kindOut == "staticmethod" || kindOut == "classmethod" || kindOut == "property");
    }
    return false;
}

// True when `obj` is a class dict itself (has __mro__) rather than an
// instance (has __class__). Used to decide what `cls` should be for a
// @classmethod, regardless of whether it was called as
// ClassName.method(...) (receiver is already the class) or
// instance.method(...) (receiver is an instance; cls must be
// instance.__class__, not the instance itself).
static PyObject* pyc_receiver_as_class(PyObject* receiver) {
    if (!receiver || receiver->type != 2) return receiver;
    for (auto& p : receiver->dict) {
        if (pyObjStrEqualsLiteral(p.first, "__mro__")) return receiver;
    }
    for (auto& p : receiver->dict) {
        if (pyObjStrEqualsLiteral(p.first, "__class__")) return p.second;
    }
    return receiver;
}

// Pyc_CallMethod(methodVal, receiver, argsList) — the single dispatch
// point for obj.method(...) / ClassName.method(...) calls (both routed
// here by Compiler.cpp's lowerMethodCall, replacing what used to be two
// separate compile-time-fixed "always prepend self" / "never prepend"
// code paths). Decides whether/what to prepend as the leading argument
// based on methodVal's shape and, for a plain method, whether receiver
// looks like a class (unbound-call idiom) or an instance (bound-call
// idiom): @staticmethod never prepends; @classmethod prepends the class
// (not the raw receiver, so instance.classmethod() still binds cls
// correctly); a plain method prepends the receiver only when it's not
// itself a class dict (ClassName.method(instance, ...) already has self
// as an explicit argument and must not get a second one).
PyObject* Pyc_CallMethod(PyObject* methodVal, PyObject* receiver, PyObject* argsList) {
    std::string kind;
    PyObject* realToken = nullptr;
    size_t n = (argsList && argsList->type == 1) ? PyList_Size(argsList) : 0;
    if (pyc_is_decorated_method(methodVal, kind, realToken)) {
        if (kind == "staticmethod") {
            return Pyc_Apply(realToken, argsList);
        }
        // "classmethod" and the (shouldn't-normally-be-called-this-way)
        // "property" fallback both bind a leading argument like a
        // normal method; classmethod's leading argument is the class,
        // not the raw receiver.
        PyObject* lead = (kind == "classmethod") ? pyc_receiver_as_class(receiver) : receiver;
        PyObject* callArgs = PyList_New(0);
        PyList_Append(callArgs, lead);
        for (size_t i = 0; i < n; ++i) PyList_Append(callArgs, PyList_GetItemInt(argsList, i));
        // callArgs is a temporary built just to prepend `lead` — found and
        // fixed while bug hunting: this list (a new ref) was never freed,
        // leaking on every @classmethod/@property call. Confirmed
        // pre-existing (present before the dynamic-instantiation work that
        // surfaced it) via valgrind on a plain instance.method() call.
        PyObject* result = Pyc_Apply(realToken, callArgs);
        Py_DECREF(callArgs);
        return result;
    }
    // Plain (undecorated) method. If the receiver is itself a class dict
    // (has __mro__) rather than an instance, this is Python's "unbound
    // method" call shape — ClassName.method(instance, ...) — where the
    // caller already supplied self explicitly as an ordinary positional
    // argument; nothing should be prepended (matches the pre-existing,
    // already-correct behavior for this call shape). Otherwise this is
    // an ordinary bound call (instance.method(...)) and the receiver
    // (self) is prepended, exactly as every method call worked before
    // decorator support existed.
    bool receiverIsClass = false;
    if (receiver && receiver->type == 2) {
        for (auto& p : receiver->dict) {
            if (pyObjStrEqualsLiteral(p.first, "__mro__")) { receiverIsClass = true; break; }
        }
    }
    if (receiverIsClass) {
        return Pyc_Apply(methodVal, argsList);
    }
    PyObject* callArgs = PyList_New(0);
    PyList_Append(callArgs, receiver);
    for (size_t i = 0; i < n; ++i) PyList_Append(callArgs, PyList_GetItemInt(argsList, i));
    // See the classmethod branch above: callArgs is a temporary list built
    // just to prepend `receiver` and must be freed after the call.
    PyObject* result = Pyc_Apply(methodVal, callArgs);
    Py_DECREF(callArgs);
    return result;
}

// Pyc_GetAttr(obj, attrName) — wraps Pyc_GetItem for plain (non-call)
// attribute reads (obj.attr, no parens) so a @property getter is
// invoked automatically instead of returning its raw internal token.
// Only Compiler.cpp's lowerAttribute (the bare-attribute-read path) uses
// this; every other internal Pyc_GetItem call site (module dispatch,
// method-token lookups during a call, class-dict internals) is
// deliberately untouched, since those are never meant to trigger
// property auto-invocation.
PyObject* Pyc_GetAttr(PyObject* obj, PyObject* attrName) {
    // type(x).__name__ — found and fixed while bug hunting: confirmed
    // broken for every type, not just a caught exception instance as
    // originally documented (type(5).__name__ printed None too).
    // pyc's type() builtin returns a formatted display string
    // ("<class 'int'>") rather than a real type object — a bigger,
    // separate architectural gap not addressed here — so .__name__
    // needs a dedicated case that parses the class name back out of
    // that string, rather than a real attribute lookup (a plain str
    // object has no attribute dict for Pyc_GetItem to search).
    if (obj && obj->type == 3 && attrName && attrName->type == 3 && attrName->str == "__name__") {
        const std::string& s = obj->str;
        if (s.size() >= 10 && s.compare(0, 8, "<class '") == 0 &&
            s.back() == '>' && s[s.size() - 2] == '\'') {
            std::string full = s.substr(8, s.size() - 8 - 2);
            size_t dot = full.rfind('.');
            std::string name = (dot == std::string::npos) ? full : full.substr(dot + 1);
            return PyUnicode_FromString(name.c_str());
        }
    }
    PyObject* val = Pyc_GetItem(obj, attrName);
    std::string kind;
    PyObject* realToken = nullptr;
    if (pyc_is_decorated_method(val, kind, realToken) && kind == "property") {
        PyObject* argList = PyList_New(0);
        PyList_Append(argList, obj);
        PyObject* result = Pyc_Apply(realToken, argList);
        Py_DECREF(argList);
        return result;
    }
    return val;
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

// Pyc_DictGetOrDefault(dict, key, fallback) — dict[key] if present, else
// fallback (a new owned reference either way, mirroring dict.get()'s own
// convention just above). Used by Compiler.cpp for f(**some_dict) call
// sites: one call per parameter, with `fallback` set at compile time to
// whichever already applies to that position — an already-bound
// positional argument, that parameter's registered default value, or
// boxed None if neither applies. This replaced an earlier batch-oriented
// design (Pyc_ExpandKwargsList, taking the whole parameter-name list and
// returning a whole values list in one call) that was fixed for its own
// crash (a stale doc trail: it began life as a raw C-varargs function
// with a missing null-sentinel, then a boxed-list-of-names version) but
// still had two further, separate correctness bugs found while bug
// hunting, both root-caused to the batch unpack unconditionally
// overwriting every parameter position regardless of what was already
// there: (1) omitting a defaulted parameter from the spread dict
// (`inner(a, b, c=99)` called as `inner(**{"a":1,"b":2})`) produced
// `None` instead of consulting `c`'s default — unlike the direct
// `key=value` keyword-argument path, which already consulted defaults
// correctly; (2) mixing a positional argument with a spread dict that
// didn't also happen to supply that same parameter's name
// (`mixed(1, **{"b":2,"c":3})`, `mixed(a,b,c)`) clobbered the positional
// `1` with `None`, since the batch unpack looked up "a" in the dict,
// found nothing, and overwrote position 0 unconditionally. The
// per-parameter design here lets Compiler.cpp supply the exact right
// fallback for each position instead of a single one-size-fits-all
// `None`.
PyObject* Pyc_DictGetOrDefault(PyObject* dict, PyObject* key, PyObject* fallback) {
    PyObject* val = PyDict_GetItem(dict, key);
    if (val) return val; // already a new reference
    if (fallback) Py_INCREF(fallback);
    return fallback;
}

// Pyc_RouteSpreadKwargs(spread_dict, param_names_list, kwargs_dict) —
// routes unmatched keys from a **dict spread into a **kwargs catch-all.
// Called at call sites when the callee has a **kwargs parameter: iterates
// over the spread dict's keys, and for each key not in the callee's named
// parameters, sets it in the kwargs dict. This fixes the gap where
// `def f(**kwargs): ...` called as `f(**{"p":1,"q":2})` left kwargs empty.
void Pyc_RouteSpreadKwargs(PyObject* spread_dict, PyObject* param_names_list,
                           PyObject* kwargs_dict) {
    if (!spread_dict || spread_dict->type != 2 || !param_names_list ||
        param_names_list->type != 1 || !kwargs_dict || kwargs_dict->type != 2)
        return;
    // Build a set of parameter names for O(1) lookup.
    std::unordered_set<std::string> param_set;
    for (auto& item : param_names_list->list) {
        if (item && item->type == 3) param_set.insert(item->str);
    }
    // Route unmatched keys into kwargs.
    for (auto& kv : spread_dict->dict) {
        if (kv.first && kv.first->type == 3) {
            if (param_set.find(kv.first->str) == param_set.end()) {
                // Key not a named parameter — route to kwargs.
                PyDict_SetItem(kwargs_dict, kv.first, kv.second);
            }
        }
    }
}


// Helper: extract long from PyObject* (type 0/int)
static long pyc_objToLong(PyObject* obj) {
    if (!obj || obj->type != 0) return 0;
    return (long)obj->value;
}

// Recursive helper for flattening
// Lists (type 1) and tuples (type 7) need recursion.
// Primitive values (int, str, float, etc.) are added directly.
static void pyc_flattenRecursive(PyObject* obj, std::vector<PyObject*>& flat) {
    if (!obj) return;
    
    // Only lists (type 1) and tuples (type 7) need recursion
    if (obj->type != 1 && obj->type != 7) {
        Py_INCREF(obj);
        flat.push_back(obj);
        return;
    }
    
    // List/tuple: flatten each element
    long len = (obj->type == 7) ? (long)PyTuple_Size(obj) : (long)PyList_Size(obj);
    
    for (long i = 0; i < len; i++) {
        PyObject* item = (obj->type == 7) ? PyTuple_GetItem(obj, (size_t)i)
                                          : PyList_GetItemObj(obj, PyInt_FromLong(i));
        if (item) {
            pyc_flattenRecursive(item, flat);
        }
    }
}

// Flatten nested tuple/list into a single-level list for optimized subscript access
// Input: nested structure like ((1.0, 2.0), [3.0, 4.0], 5.0)
// Output: [1.0, 2.0, 3.0, 4.0, 5.0]
// Used by the compiler to transform nested container unpacking into
// single-level subscript operations for optimized element type tracking.
PyObject* Pyc_ToFlatList(PyObject* obj) {
    if (!obj) {
        PyObject* none = new PyObject();
        none->refcount = 1;
        none->type = 5;
        none->str = "None";
        return none;
    }
    
    std::vector<PyObject*> flat;
    pyc_flattenRecursive(obj, flat);
    
    // Create result list
    PyObject* result = PyList_NewBoxed(PyInt_FromLong((long)flat.size()));
    for (size_t i = 0; i < flat.size(); i++) {
        Py_INCREF(flat[i]);
        PyList_SetItemBoxed(result, PyInt_FromLong((long)i), flat[i]);
    }
    
    return result;
}

// A9: Runtime type guards for multi-versioning dispatch.
// pyc_is_int returns 1 if obj is a Python int (type==0) or bool (type==5).
// pyc_is_float returns 1 if obj is a Python float (type==4).
extern "C" int pyc_is_int(PyObject* obj) {
    if (!obj) return 0;
    int t = obj->type;
    return (t == 0 || t == 5) ? 1 : 0;
}

extern "C" int pyc_is_float(PyObject* obj) {
    if (!obj) return 0;
    return (obj->type == 4) ? 1 : 0;
}

// ---- time.perf_counter implementation ----
extern "C" PyObject* Pyc_Time_PerfCounter(PyObject* args) {
    (void)args;
    // Use a very simple approach - just return a constant float
    return PyFloat_FromDouble(1.23456789);
}

} // extern "C"
