# corpus case — ground truth is CPython at run time (CHARTER I5).
class B:
    def __init__(self): self.s = 'x'
b = B()
b.s += 'yz'
print(b.s)
