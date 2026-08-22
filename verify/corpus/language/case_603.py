# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(a, b):
    return a
try:
    print(f(1))
except TypeError as e:
    print(type(e).__name__ + ":", e)
