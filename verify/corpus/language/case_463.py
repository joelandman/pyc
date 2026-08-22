# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(x):
    return x + 1
g = f
print(g(4))
print(g is f)
print(g == f)
def h(x):
    return x - 1
print(f == h)
