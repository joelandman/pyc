# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def f(self):
        s = super()
        return s.count(1)
try:
    print(C().f())
except AttributeError as e:
    print(type(e).__name__)
