# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: PEP 695 type-param shadowing and scoping rules. The type param name
# may shadow a value at module level; inside a function it binds a type scope.
from typing import TypeVar

# module-level value and type param with the same name — allowed in 3.13+
type Alias = int


def use(v: Alias) -> Alias:
    return v


print(use(5))


# type param shadowing a plain name inside generic class body
class Node[T]:
    data: T


# PEP 695 type alias used in a subscript, redefined
type Point = tuple[int, int]
Point2 = Point
print(Point2)
print(Point(1, 2))
