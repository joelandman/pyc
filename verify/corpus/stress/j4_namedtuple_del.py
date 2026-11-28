# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: namedtuples (both collections and typing), zip(strict=), del on
# subscript/attr/name, and __future__ annotations.
from collections import namedtuple
from typing import NamedTuple


Point = namedtuple("Point", ["x", "y"])
p = Point(1, 2)
print(p.x, p[1], p._replace(x=10))


class Pt2(NamedTuple):
    x: int
    y: int


print(Pt2(3, 4)._fields)

# zip strict: mismatch raises on both sides identically
def strict_ok():
    return list(zip([1, 2, 3], [4, 5, 6], strict=True))


print(strict_ok())

try:
    list(zip([1, 2], [1, 2, 3], strict=True))
    print("no-err")
except ValueError:
    print("ValueError")


# del on subscript, attribute, and name
d = {"a": 1, "b": 2}
del d["a"]
print(sorted(d))


class Obj:
    def __init__(self):
        self.keep = 1
        self.drop = 2


o = Obj()
del o.drop
print(hasattr(o, "keep"), not hasattr(o, "drop"))

x = 10
del x
try:
    print(x)
    print("still-here")
except NameError:
    print("NameError")
