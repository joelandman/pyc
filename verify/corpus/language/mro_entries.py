from typing import NamedTuple
class Point(NamedTuple):
    x: int
    y: int
p = Point(1, 2)
print(p.x, p.y, type(p).__name__)
print(Point.__bases__[0].__name__, type(Point.__orig_bases__[0]).__name__)
