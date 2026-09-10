import inspect

def f(x, y=1):
    pass
print(str(inspect.signature(f)))

def g(a, /, b, *c, d=1, **e):
    pass
print(str(inspect.signature(g)))

def h(x: int, y: str = "a") -> list:
    pass
print(str(inspect.signature(h)))

def k(*, z=2):
    pass
print(str(inspect.signature(k)))

class C:
    def m(self, x):
        pass
print(str(inspect.signature(C.m)))
print(str(inspect.signature(C().m)))
