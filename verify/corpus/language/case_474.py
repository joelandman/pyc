# corpus case — ground truth is CPython at run time (CHARTER I5).
def f():
    try:
        return 1
    finally:
        print("fin1")
print(f())
def g():
    try:
        return 5
    except ValueError:
        return -1
print(g())
try:
    raise KeyError("k")
except KeyError:
    print("stack ok")
