# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: t-string inside f-string and vice-versa, f-string of a t-string.
from typing import TypeVar
T = TypeVar("T")

# t-string of a generic — must read the TypeVar
def wrap(x: T) -> T:
    return x

s = f"wrap({wrap})"
t = f"{T}"
print(s)
print(t)

# f-string interpolating a t-string's value
tv = t"inner"
print(f"outer:{tv}")
print(f"{t'x={1}'}")
