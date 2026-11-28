# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: dataclass basic generation, defaults, repr, eq, field reorder.
from dataclasses import dataclass, field


@dataclass
class Point:
    x: int = 0
    y: int = 0
    label: str = "origin"


p = Point(1, 2, "a")
print(p)
print(p == Point(1, 2, "a"))
print(Point() == Point())
print(repr(p).startswith("Point(x=1"))
