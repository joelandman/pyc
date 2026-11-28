# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: PEP 695 type params (generic class, generic method, type alias).
from typing import TypeVar, Generic

T = TypeVar("T")


class Box(Generic[T]):
    def __init__(self, value: T) -> None:
        self.value = value

    def get(self) -> T:
        return self.value


class Node[T]:
    def method[T](self, x: T) -> T:
        return x


type Pair = tuple[int, int]


print(Box(5).get())
print(Node().method("hi"))
print(Pair(1, 2))
