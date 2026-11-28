# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: __future__ annotations and PEP 563 deferred evaluation of
# annotations, plus a forward-referenced annotation in a function signature.
from __future__ import annotations


class Future:
    pass


def takes_future(x: Future) -> Future:
    return x


# the annotation is a string at runtime (PEP 563), so Future need not exist
print(takes_future.__annotations__)
print(takes_future.__annotations__["x"])


# annotations referencing names in a string forward ref
def f(a: "undefined_name_xyz", b: list[int]) -> int:
    return b[0]


print(f.__annotations__)
