# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def foo(self, a=1, b=2):
        return (a, b)
print(C().foo())
def f(x, a=1, b=2):
    return (a, b)
g = f
print(g(0))
