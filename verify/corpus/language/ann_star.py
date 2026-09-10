from typing import TypeVarTuple, Unpack
Ts = TypeVarTuple("Ts")
def f(*args: *Ts):
    pass
print(f.__annotations__["args"] == Unpack[Ts])
try:
    f.__annotate__(3)
    print("format3")
except NotImplementedError:
    print("format3 nie")
