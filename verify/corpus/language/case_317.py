# corpus case — ground truth is CPython at run time (CHARTER I5).
def inner(a, b, c):
    return a + b + c
d = {'a': 1, 'b': 2, 'c': 3}
print(inner(**d))
