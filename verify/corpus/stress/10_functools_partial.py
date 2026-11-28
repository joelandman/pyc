# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: functools.partial, args/kwargs composition, keyword overriding.
from functools import partial


def add(a, b, c):
    return a + b + c


p1 = partial(add, 1)
p2 = partial(add, 1, 2)
print(p1(3))
print(p2(3))
print(partial(add, c=10, a=1, b=2)())
print(partial(add, 1, 2, 3)(4))
