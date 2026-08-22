# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def foo(self, a):
        return a
try:
    C().foo()
except TypeError as e:
    print(e)
