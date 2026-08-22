# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(a, b, **kwargs):
    print(a, b, sorted(kwargs.keys()), kwargs['x'], kwargs['y'])
f(1, 2, x=10, y=20)
def f2(a, b, **kwargs):
    print(a, b, len(kwargs))
f2(1, 2)
