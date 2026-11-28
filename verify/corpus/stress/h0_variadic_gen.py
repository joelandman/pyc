# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: PEP 646 variadic generic syntax. TypeVarTuple, Unpack, and the
# StarTypeVar. If pyc refuses this on valid 3.14 source, that is the finding.
from typing import TypeVar, TypeVarTuple, Unpack

Ts = TypeVarTuple("Ts")
T = TypeVar("T")


class Matrix(Generic := __import__("typing").Generic):
    pass


def first_row(
    m: list[Unpack[Ts]],
) -> Unpack[Ts]:
    ...


Ts2 = TypeVarTuple("Ts2")


class Table[Ts2]:  # PEP 695 class with a type-param-tuple
    rows: list[list[Unpack[Ts2]]]


print(Table)
print(T)
