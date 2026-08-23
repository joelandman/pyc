def f():
    """hello"""
    return 3
print(f.__name__, f.__doc__)
class C:
    x = 7
    def g(self, k, default=x): return default
print(C().g("k"))
import functools
def deco(fn):
    @functools.wraps(fn)
    def w(*a, **k): return fn(*a, **k)
    return w
@deco
def h(a): return a*2
print(h(21), h.__name__)
class D: pass
def m(self): return "late"
D.m = m
print(D().m())
