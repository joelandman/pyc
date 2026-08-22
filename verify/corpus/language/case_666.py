# corpus case — ground truth is CPython at run time (CHARTER I5).
def outer():
    class C42:
        def get(self, k, default=42):
            return default
    return C42().get("x")
print(outer())
def outer2():
    class C05:
        def get(self, k, default=0.5):
            return default
    return C05().get("x")
print(outer2())
def outer3():
    class CInit:
        def __init__(self, n=42):
            self.n = n
    return CInit().n
print(outer3())
class CAttr:
    x = 7
    def get(self, k, default=x):
        return default
print(CAttr().get("x"))
