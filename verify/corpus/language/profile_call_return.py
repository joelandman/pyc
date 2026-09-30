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
