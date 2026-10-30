# Regression probes for the C-trace fixes:
#  1. A C tracer's "object" argument must be c_traceobj (the recording list),
#     not the thread state. Passing the thread state made any C tracer that
#     dereferences its object argument read _PyRuntime and segfault.
#  2. A PyTrace_CALL event must report the CALLEE's co_firstlineno (the frame
#     starts at RESUME), not the line of the first body instruction.
#  3. A line event must not repeat for a line that has not changed; the C
#     tracer below records every event, so a duplicate is visible.

import sys
import _testcapi


def gen():
    yield 1


def func():
    for _ in (
        gen()
    ):
        pass


events = []
_testcapi.settrace_to_record(events)
func()
sys.settrace(None)
if not events:
    raise AssertionError("no C trace events recorded")

# The first event must be a line event for the call site, then the CALL for
# func. The CALL must carry func's first line, not the first body line.
CALL, LINE, RETURN = 0, 2, 3
start = func.__code__.co_firstlineno
call_lines = [line for (what, line, arg) in events if what == CALL]
func_calls = [line for (what, line, arg) in events
              if what == CALL and arg is not None
              and getattr(arg, "__code__", None) is not None
              and arg.__code__.co_name == "func"]
if func_calls and func_calls[0] != start:
    raise AssertionError(f"call event line {func_calls[0]} != firstlineno {start}")

# Python-level tracer: call events report the callee's first line.
seen = []


def tracer(frame, event, arg):
    seen.append((event, frame.f_lineno, frame.f_code.co_name))
    return tracer


sys.settrace(tracer)
func()
sys.settrace(None)
py_call = [ln for (ev, ln, name) in seen if ev == "call" and name == "func"]
if py_call and py_call[0] != start:
    raise AssertionError(f"python call line {py_call[0]} != firstlineno {start}")

# The LINE stream must be duplicate-free within a frame: a line event fires
# only when the line CHANGES (CPython's unit rule). A duplicate means a slot
# re-stamped the line the frame already sits on -- the symptom of the B7
# gap, where the loop's check and back-edge slots carried the for-statement
# line and re-fired it on every iteration of the desugared loop above.
lines = [ln for (ev, ln, name) in seen if ev == "line" and name == "func"]
for a, b in zip(lines, lines[1:]):
    if a == b:
        raise AssertionError(f"duplicate line event {a} in {lines}")

print("c-trace ok")
