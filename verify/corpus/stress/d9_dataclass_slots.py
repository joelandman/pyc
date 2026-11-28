# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: slots dataclass, @unique enum, nested comprehension capturing loop var.
from dataclasses import dataclass
from enum import Enum, unique


@dataclass(slots=True)
class Pt:
    x: int
    y: int


p = Pt(1, 2)
print(p.x, p.y)
print(p == Pt(1, 2))


@unique
class C(Enum):
    A = 1
    B = 2


print(C.A, list(C))


def matrix(n):
    return [[i * n + j for j in range(n)] for i in range(n)]


print(matrix(3))
