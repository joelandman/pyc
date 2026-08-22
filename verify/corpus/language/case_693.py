# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def f(self, a):
        return a
    def g(self, a, b=2):
        return (a, b)
print(C().f(a=1))
print(C().f(**{"a": 2}))
print(C().g(a=1, b=3))
print(C().f(3))
try:
    print(C().f(a=1, x=9))
except TypeError as e:
    print(type(e).__name__)
