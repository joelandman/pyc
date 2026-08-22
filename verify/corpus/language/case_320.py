# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(a, b, c=5):
    return a, b, c
x, y, z = f(**{'a': 1}, b=2)
print(x, y, z)
