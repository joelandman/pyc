// --trace core: Perfetto (Chrome-JSON) trace of the program's execution.
//
// Linked only when pyc is invoked with --trace (driver.cpp adds the TU and
// the PYC_WITH_TRACE define for entry.cpp). Without the PYC_TRACE env var
// both hooks are a single getenv and the binary behaves identically to an
// unflagged build (gated invariant, verify.yml). With PYC_TRACE=<path> set,
// the binary appends one JSON object per line (the Chrome trace-event
// format, imported natively by https://ui.perfetto.dev).
//
// Event model, measured evidence, and cost: rebuild/TRACING.md.
#ifndef PYC_RT_TRACE_HPP
#define PYC_RT_TRACE_HPP

#ifdef __cplusplus
extern "C" {
#endif

// Call after the interpreter is ready and before the module body.
// Reads PYC_TRACE (path) and PYC_TRACE_LINES (1 = also fire LINE instants).
// No-op when PYC_TRACE is unset/empty. Never aborts the program: a failed
// bootstrap prints a `pyc: trace:` diagnostic and leaves tracing off.
void pyc_rt_trace_init(void);

// Call after the module body (an atexit backstop covers abnormal paths).
// Closes any open slices so the file is always balanced, flushes, and
// frees the tool. Idempotent.
void pyc_rt_trace_end(void);

#ifdef __cplusplus
}
#endif
#endif
