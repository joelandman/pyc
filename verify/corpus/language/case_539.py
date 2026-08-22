# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(**kwargs):
    print(kwargs.get("a"), kwargs.get("b"), len(kwargs))

g = f
g(a=1, b=2)
g()

def h(a, **kwargs):
    print(a, kwargs.get("x"), kwargs.get("y"), len(kwargs))

j = h
j(1, x=2, y=3)
j(5)

def both(*args, **kwargs):
    return args, kwargs

k = both
r1 = k(1, 2, 3, a=4, b=5)
print(len(r1[0]), r1[0][0], r1[0][1], r1[0][2], r1[1].get("a"), r1[1].get("b"), len(r1[1]))
r2 = k(1, 2, 3)
print(len(r2[0]), len(r2[1]))
r3 = k(a=1)
print(len(r3[0]), r3[1].get("a"))
