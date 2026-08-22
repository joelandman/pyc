# corpus case — ground truth is CPython at run time (CHARTER I5).
def f():
    """hello"""
    return 3
print(f.__name__)
print(f.__doc__)
print(f.__call__())
g = lambda: 1
print(g.__name__)
print(g.__doc__)
