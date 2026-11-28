# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: cached_property caching the computed value across accesses.
from functools import cached_property


class Expensive:
    def __init__(self):
        self.count = 0

    @cached_property
    def value(self):
        self.count += 1
        return sum(range(100))


e = Expensive()
print(e.value)
print(e.value)
print(e.value)
print(e.count)
