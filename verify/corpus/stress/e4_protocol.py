# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: typing.Protocol structural subtyping at runtime.
from typing import Protocol


class Flyer(Protocol):
    def fly(self) -> str: ...


class Bird:
    def fly(self) -> str:
        return "flap"


def describe(f: Flyer) -> str:
    return f.fly()


print(describe(Bird()))
