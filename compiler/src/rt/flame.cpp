// --flamegraph core: sampling profiler + self-contained SVG writer.
// See flame.hpp and rebuild/TRACING.md for the design and measurements.
//
// Sampling discipline: the sampler thread wakes at the configured rate,
// takes the GIL (PyGILState_Ensure -- the same discipline CPython's own
// sys._current_frames / faulthandler rely on), and walks the MAIN
// thread's frame chain via the public C API only:
//   _PyThread_CurrentFrames()  (the C implementation of
//                               sys._current_frames(): dict tid -> frame)
//   PyFrame_GetBack(frame)     (new reference)
//   frame.f_code  attribute -> PyCodeObject.co_qualname (public fields)
// The main thread is GIL-blocked while the walk runs, so the chain is
// quiescent. A signal-based async-signal-safe walker was considered and
// rejected for v1: it can read frames mid-teardown.
//
// A sample taken while the main thread is inside C code that released
// the GIL (numpy, ...) attributes the time to the Python frame that made
// the C call -- as in any Python-level sampler. For the C interior use
// --trace (C: slices) or perf.
//
// Only the sampler thread writes the aggregation map; the writer reads
// it after join(). No locks anywhere.

#include "pyc/rt/flame.hpp"

#include <Python.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kMaxFrames = 256;
constexpr int kW = 1200;
constexpr int kRowH = 17;
constexpr int kPadTop = 16;
constexpr int kPadBottom = 8;

std::atomic<bool> g_stop{false};
bool g_running = false;
std::thread g_thread;
unsigned long g_main_tid = 0;
uint64_t g_hz = 99;
char g_path[4096];
std::map<std::string, uint64_t> g_samples;  // folded stack -> count
uint64_t g_first_ns = 0;
uint64_t g_last_ns = 0;

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void take_sample() {
    PyObject* frames = _PyThread_CurrentFrames();
    if (!frames) {
        PyErr_Clear();
        return;
    }
    std::vector<std::string> stk;
    {
        PyObject* key = PyLong_FromUnsignedLong(g_main_tid);
        PyObject* fobj = PyDict_GetItemWithError(frames, key);  // borrowed
        if (fobj && key) {
            // The dict entry is borrowed; PyFrame_GetBack results are owned.
            PyFrameObject* f = (PyFrameObject*)fobj;
            PyFrameObject* prev = nullptr;
            while (f && stk.size() < (size_t)kMaxFrames) {
                PyObject* code = PyObject_GetAttrString((PyObject*)f, "f_code");
                if (code) {
                    PyCodeObject* co = (PyCodeObject*)code;
                    const char* q =
                        co->co_qualname ? PyUnicode_AsUTF8(co->co_qualname) : nullptr;
                    stk.emplace_back(q ? q : "?");
                    Py_DECREF(code);
                } else {
                    PyErr_Clear();
                    stk.emplace_back("?");
                }
                PyFrameObject* back = PyFrame_GetBack(f);  // new reference
                Py_XDECREF(prev);
                prev = back;
                f = back;
            }
            Py_XDECREF(prev);
        }
        if (key) Py_DECREF(key);
    }
    Py_DECREF(frames);
    if (stk.empty()) return;  // main thread not in Python (C with GIL released)
    // The walk goes innermost -> outermost; the folded-stack convention is
    // outermost (root) first, so reverse.
    std::string folded;
    for (auto it = stk.rbegin(); it != stk.rend(); ++it) {
        if (!folded.empty()) folded += ';';
        folded += *it;
    }
    g_samples[folded]++;
    uint64_t ns = now_ns();
    if (!g_first_ns) g_first_ns = ns;
    g_last_ns = ns;
}

void sampler() {
    const long long ns_interval = 1000000000LL / (long long)g_hz;
    while (!g_stop.load(std::memory_order_relaxed)) {
        // The deadline is RECOMPUTED each iteration, never accumulated: if
        // the GIL acquisition below blocks longer than one interval (the
        // main thread holds it), a deadline accumulated from before the
        // block lands in the past and clock_nanosleep returns immediately --
        // turning the sampler into a GIL-ping-pong spin that starves the
        // program (measured: >100x slowdown on nbody, 2026-11-08). An early
        // wake (signal) re-sleeps the full interval: at most one tick of
        // drift, immaterial for a profiler.
        struct timespec now, req;
        clock_gettime(CLOCK_MONOTONIC, &now);
        req = now;
        req.tv_nsec += ns_interval;
        while (req.tv_nsec >= 1000000000L) {
            req.tv_nsec -= 1000000000L;
            req.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, nullptr);
        if (g_stop.load(std::memory_order_relaxed)) break;
        PyGILState_STATE st = PyGILState_Ensure();
        take_sample();
        PyGILState_Release(st);
    }
}

// --- SVG writer -----------------------------------------------------------

uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

void xml_escape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
}

struct Node {
    uint64_t total = 0;
    uint64_t self = 0;
    std::map<std::string, Node> kids;  // sorted by name: deterministic layout
};

int max_depth(const Node& n, int d) {
    int best = d;
    for (auto& [k, c] : n.kids) best = max_depth(c, d + 1);
    return best;
}

void render(const Node& n, const std::string& name, uint64_t grand_total, int depth,
            double xfrac, double wfrac, std::string& out, int H) {
    int w = (int)(wfrac * (double)kW);
    if (w < 1) return;  // sub-pixel: skip (title text would not fit anyway)
    int x = (int)(xfrac * (double)kW);
    int y = H - kPadBottom - (depth + 1) * kRowH;  // root at the bottom
    std::string nm;
    xml_escape(nm, name);
    std::string title = nm;
    char line[128];
    std::snprintf(line, sizeof line, "%s: %llu samples (%.1f%% self, %.1f%% total)",
                  name.c_str(), (unsigned long long)n.self,
                  grand_total ? 100.0 * (double)n.self / (double)grand_total : 0.0,
                  grand_total ? 100.0 * (double)n.total / (double)grand_total : 0.0);
    std::string t2;
    xml_escape(t2, line);
    unsigned hue = fnv1a(name.c_str()) % 360u;
    out += "<g><title>";
    out += t2;
    out += "</title>\n<rect x=\"";
    out += std::to_string(x);
    out += "\" y=\"";
    out += std::to_string(y);
    out += "\" width=\"";
    out += std::to_string(w);
    out += "\" height=\"16\" rx=\"2\" fill=\"hsl(";
    out += std::to_string(hue);
    out += ",62%,74%)\" />\n";
    if (w > 40) {
        // ~6 px/char at 11 px monospace
        size_t maxchars = (size_t)((w - 6) / 6);
        std::string label = name.size() > maxchars ? name.substr(0, maxchars - 1) + "…"
                                                   : name;
        std::string esc;
        xml_escape(esc, label);
        out += "<text x=\"";
        out += std::to_string(x + 3);
        out += "\" y=\"";
        out += std::to_string(y + 12);
        out += "\" fill=\"#000\" font-family=\"monospace\" font-size=\"11\">";
        out += esc;
        out += "</text>\n";
    }
    out += "</g>\n";
    double cum = xfrac;
    for (auto& [k, c] : n.kids) {
        double cw = grand_total ? (double)c.total / (double)grand_total : 0.0;
        render(c, k, grand_total, depth + 1, cum, cw, out, H);
        cum += cw;
    }
}

bool write_svg(const char* path) {
    Node root;
    for (auto& [folded, count] : g_samples) {
        Node* cur = &root;
        size_t start = 0;
        for (size_t end = folded.find(';');; end = folded.find(';', start)) {
            std::string part =
                folded.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (end == std::string::npos) {
                // Innermost frame: the leaf carries the self time; its parent
                // (and all above) still count the sample in their totals.
                cur->total += count;
                Node* leaf = &cur->kids[part];
                leaf->total += count;
                leaf->self += count;
                break;
            }
            cur->total += count;
            cur = &cur->kids[part];
            start = end + 1;
        }
    }
    uint64_t total = root.total;
    int depth = max_depth(root, 0);
    int H = kPadTop + (depth + 1) * kRowH + kPadBottom;
    uint64_t ms = (g_last_ns > g_first_ns) ? (g_last_ns - g_first_ns) / 1000000ull : 0;

    std::string out;
    out += "<?xml version=\"1.0\" standalone=\"no\"?>\n";
    out += "<!-- pyc flamegraph: ";
    out += std::to_string(total);
    out += " samples, ";
    out += std::to_string(g_samples.size());
    out += " unique stacks, ";
    out += std::to_string(g_hz);
    out += " Hz, sampling window ";
    out += std::to_string(ms);
    out += " ms -->\n";
    out += "<svg version=\"1.1\" height=\"";
    out += std::to_string(H);
    out += "\" width=\"";
    out += std::to_string(kW);
    out += "\" xmlns=\"http://www.w3.org/2000/svg\">\n";
    out += "<rect width=\"100%\" height=\"100%\" fill=\"#fff\" />\n";
    if (total == 0) {
        out += "<text x=\"20\" y=\"30\" fill=\"#900\" font-family=\"monospace\" "
              "font-size=\"13\">no samples at ";
        out += std::to_string(g_hz);
        out += " Hz -- the program finished before the first tick; retry with "
               "PYC_FLAMEGRAPH_HZ=999</text>\n";
    } else {
        render(root, "all", total, 0, 0.0, 1.0, out, H);
    }
    out += "</svg>\n";

    FILE* fp = std::fopen(path, "w");
    if (!fp) {
        std::fprintf(stderr, "pyc: flamegraph: cannot open '%s' for writing\n", path);
        return false;
    }
    std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    return true;
}

}  // namespace

extern "C" {

void pyc_rt_flame_init(void) {
    const char* path = std::getenv("PYC_FLAMEGRAPH");
    if (!path || !*path || g_running) return;
    const char* hz = std::getenv("PYC_FLAMEGRAPH_HZ");
    if (hz) {
        long v = std::atol(hz);
        if (v >= 1 && v <= 10000) g_hz = (uint64_t)v;
    }
    std::snprintf(g_path, sizeof g_path, "%s", path);
    PyThreadState* ts = PyThreadState_Get();
    g_main_tid = ts ? ts->thread_id : 0;
    g_stop.store(false, std::memory_order_relaxed);
    g_thread = std::thread(sampler);
    g_running = true;
    std::atexit(pyc_rt_flame_end);
}

void pyc_rt_flame_end(void) {
    if (!g_running) return;
    g_running = false;
    g_stop.store(true, std::memory_order_relaxed);
    // The main thread holds the GIL here. Release it for the join: if the
    // sampler is blocked in PyGILState_Ensure, it waits on the GIL that the
    // main thread only releases later in Py_FinalizeEx, while the main
    // blocks in join() -- a deadlock. Measured in the Ensure window (~5 ms of
    // each ~15 ms tick on nbody, 2026-11-08): ~1 in 3 runs hung before the
    // fix.
    PyThreadState* ts = PyThreadState_Get();
    if (ts) PyEval_SaveThread();
    g_thread.join();
    if (ts) PyEval_RestoreThread(ts);
    write_svg(g_path);
    g_samples.clear();
}

}  // extern "C"
