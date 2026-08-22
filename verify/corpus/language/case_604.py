# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(a, b=2):
    return a, b
try:
    print(f())
except TypeError as e:
    print(type(e).__name__ + ":", e)
print(f(1))
