# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: IntEnum numeric interop, comparison, arithmetic.
from enum import IntEnum


class Rank(IntEnum):
    LOW = 1
    HIGH = 2


print(Rank.HIGH + 10)
print(Rank.HIGH > Rank.LOW)
print(list(Rank))
print(Rank.LOW == 1)
print(sorted(Rank))
