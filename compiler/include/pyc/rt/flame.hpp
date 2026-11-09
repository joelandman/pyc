// --flamegraph core: a sampling profiler that writes a self-contained SVG
// flamegraph of the main thread's Python stack at exit.
//
// Linked only when pyc is invoked with --flamegraph (driver.cpp adds the
// TU and the PYC_WITH_FLAME define for entry.cpp). Without the
// PYC_FLAMEGRAPH env var the cost is one getenv at startup and the binary
// behaves identically to an unflagged build (gated invariant, verify.yml).
//
// With PYC_FLAMEGRAPH=<path> set (PYC_FLAMEGRAPH_HZ=N, default 99), a
// detached thread wakes at N Hz, takes the GIL, walks the main thread's
// frame chain with the public C API only, and aggregates folded stacks;
// at exit it writes the SVG. See rebuild/TRACING.md for the design and
// the measured overhead.
#ifndef PYC_RT_FLAME_HPP
#define PYC_RT_FLAME_HPP

#ifdef __cplusplus
extern "C" {
#endif

// Call after the interpreter is ready and before the module body. Spawns
// the sampler thread iff PYC_FLAMEGRAPH is set; no-op otherwise.
void pyc_rt_flame_init(void);

// Call after the module body (an atexit backstop covers abnormal paths).
// Joins the sampler and writes the SVG. Idempotent.
void pyc_rt_flame_end(void);

#ifdef __cplusplus
}
#endif
#endif
