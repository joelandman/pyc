# corpus case — ground truth is CPython at run time (CHARTER I5).
class B:
    def __init__(self, n): self.n = n
class Outer:
    def __init__(self): self.inner = B(10)
o = Outer()
o.inner.n += 100
print(o.inner.n)
