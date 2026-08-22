# corpus case — ground truth is CPython at run time (CHARTER I5).
class Counter:
    def __init__(self):
        self.n = 0
    def __call__(self, x):
        self.n += 1
        return x * 2
f = Counter()
print(f(5))
print(f.n)
print(f(10))
print(f.n)
