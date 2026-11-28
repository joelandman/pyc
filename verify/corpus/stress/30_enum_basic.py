# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: Enum member access, iteration, __members__, str(enum).
from enum import Enum


class Color(Enum):
    RED = 1
    GREEN = 2
    BLUE = 3


print(Color.RED)
print(Color.RED.value)
print(Color(1))
print(Color["BLUE"])
print([m.name for m in Color])
print(sorted(Color.__members__.keys()))
print(Color.RED == Color.RED)
try:
    Color.RED = Color.GREEN
    print("REASSIGNABLE")
except Exception as e:
    print(type(e).__name__)
