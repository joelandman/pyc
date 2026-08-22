# corpus case — ground truth is CPython at run time (CHARTER I5).
def h(*args, **kwargs):
    return len(args), len(kwargs)
a, b = h(1, 2, 3, x=1, y=2)
print(a, b)
c, d = h(1, 2, 3)
print(c, d)
e, f = h(x=1)
print(e, f)
g, h2 = h()
print(g, h2)
