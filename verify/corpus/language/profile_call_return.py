import sys

events = []

def hook(frame, event, arg):
    name = frame.f_code.co_name
    if event in ("call", "return") and name in ("g", "f", "h", "missing"):
        events.append((name, event, arg))

def f(x):
    return x + 1

def g(x):
    return f(x)

def h():
    raise ValueError("nope")

def missing(a):
    return a

def collect(fn):
    events.clear()
    sys.setprofile(hook)
    try:
        fn()
    finally:
        sys.setprofile(None)
    print("n", len(events))
    for name, event, arg in events:
        print("ev", name, event, arg)

def run_g():
    print("g", g(1))

def run_h():
    try:
        h()
    except ValueError:
        print("raised")

def run_missing():
    try:
        missing()
    except TypeError:
        print("typeerror")

collect(run_g)
collect(run_h)
collect(run_missing)

# Bound method of a C function. A plain call expands the method before the
# event; a starred call does not, so max itself produces no c_call there.
class A:
    f = classmethod(repr)

class B:
    f = classmethod(max)

def hook_all(frame, event, arg):
    events.append(event)

def show(fn):
    events.clear()
    sys.setprofile(hook_all)
    try:
        fn()
    finally:
        sys.setprofile(None)
    print("n", len(events))
    for event in events:
        print("ev", event)

def case_method():
    A().f()

def case_kw():
    B().f(1, key=lambda x: 0)

def case_ex():
    args = (1,)
    m = B().f
    m(*args, key=lambda x: 0)

show(case_method)
show(case_kw)
show(case_ex)
