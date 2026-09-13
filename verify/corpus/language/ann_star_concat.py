from typing import TypeVarTuple, Unpack, Tuple

Ts = TypeVarTuple("Ts")

def a(*args: *tuple[int, *Ts]):
    pass

ann = a.__annotations__["args"]
print("repr", repr(ann))
print("type", type(ann).__module__ + "." + type(ann).__name__)
print("eq_unpack", ann == Unpack[tuple[int, Unpack[Ts]]])
print("eq_star0", ann == (*tuple[int, *Ts],)[0])
