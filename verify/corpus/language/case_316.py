# corpus case — ground truth is CPython at run time (CHARTER I5).
class B:
    def __init__(self, n): self.n = n
b = B(5)
calls = []
def get_box():
    calls.append(1)
    return b
get_box().n += 1000
print(b.n, len(calls))
