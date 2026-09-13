from typing import Unpack, Tuple

def a(*args: *tuple[int, ...]):
    pass

ann = a.__annotations__["args"]
print("repr", repr(ann))
print("type", type(ann).__module__ + "." + type(ann).__name__)
print("eq_unpack", ann == Unpack[tuple[int, ...]])
print("eq_star0", ann == (*tuple[int, ...],)[0])
