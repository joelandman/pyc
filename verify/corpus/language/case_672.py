# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(a):
    return a
def mk():
    return []
try:
    print(f(*mk()))
except TypeError as e:
    print(type(e).__name__)
