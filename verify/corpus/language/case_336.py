# corpus case — ground truth is CPython at run time (CHARTER I5).
class Range2:
    def __init__(self, n):
        self.n = n
        self.i = 0
    def __iter__(self):
        return self
    def __next__(self):
        if self.i >= self.n:
            raise StopIteration
        v = self.i
        self.i += 1
        return v
for x in Range2(3):
    print('r:', x)
print(list(Range2(2)))
