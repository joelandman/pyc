# corpus case — ground truth is CPython at run time (CHARTER I5).
class B:
    def __init__(self, n): self.n = n
b = B(5)
b.n += 3
b.n -= 1
b.n *= 2
print(b.n)
