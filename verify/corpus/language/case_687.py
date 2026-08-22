# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(a):
    return a
hs = [f]
print(hs[0](*[1]))
def f135(a, *args, **kw):
    print(a)
    print(len(args))
    print(kw)
def mk135():
    return [1]
f135(*mk135())
def f135b(a, **kw):
    print(a)
    print(kw)
f135b(*mk135())
def mk_tup():
    return (1,)
print(f(*mk_tup()))
def g(a, b=2):
    return (a, b)
def mk():
    return [1]
print(g(*mk(), b=3))
