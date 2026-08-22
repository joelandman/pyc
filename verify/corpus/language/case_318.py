# corpus case — ground truth is CPython at run time (CHARTER I5).
def inner(a, b, c=99):
    return a + b + c
d1 = {'a': 1, 'b': 2}
print(inner(**d1))
d2 = {'a': 10, 'b': 20, 'c': 30}
print(inner(**d2))
