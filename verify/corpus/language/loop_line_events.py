# B7 ground truth: the exact (event, line) stream CPython fires for each
# desugared loop construct, plus the per-unit line table CPython assigned.
# Run this file with the oracle; run the pyc-compiled binary identically and
# diff. Only events inside the named functions are recorded (module noise
# excluded identically on both sides).

import dis
import sys

TARGETS = {"f_simple", "f_multi", "f_while", "f_break", "f_nested", "f_body"}
RECORD = []


def tracer(frame, event, arg):
    name = frame.f_code.co_name
    if name in TARGETS:
        RECORD.append((event, frame.f_lineno, name))
    return tracer


def run(name, fn):
    RECORD.clear()
    sys.settrace(tracer)
    try:
        fn()
    finally:
        sys.settrace(None)
    seq = [f"{e}@{l}" for (e, l, _n) in RECORD]
    print(f"{name}: " + " ".join(seq))


def f_simple():
    for i in range(3):
        pass


def f_multi():
    for i in (
        range(3)
    ):
        x = i


def f_while():
    i = 0
    while i < 3:
        i = i + 1


def f_break():
    for i in range(10):
        if i == 3:
            break
    else:
        pass


def f_nested():
    for i in range(2):
        for j in range(2):
            pass


def f_body():
    for i in range(2):
        x = i
        y = x + 1


if __name__ == "__main__":
    print("--- unit lines ---")
    for name in ("f_simple", "f_multi", "f_while", "f_break", "f_nested", "f_body"):
        co = globals()[name].__code__
        parts = []
        prev = None
        for ins in dis.get_instructions(co):
            line = ins.positions[0] if ins.positions else -1
            tag = f"{ins.opname}@{line}"
            if tag != prev:
                parts.append(tag)
                prev = tag
        print(f"{name}[{co.co_firstlineno}]: " + " ".join(parts))
    print("--- event streams ---")
    run("simple", f_simple)
    run("multi", f_multi)
    run("while", f_while)
    run("break", f_break)
    run("nested", f_nested)
    run("body", f_body)
