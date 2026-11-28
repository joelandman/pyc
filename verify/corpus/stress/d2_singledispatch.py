# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: functools.singledispatch dispatches on first arg type.
from functools import singledispatch


@singledispatch
def handle(arg):
    return f"default:{arg!r}"


@handle.register(str)
def _handle_str(arg):
    return f"str:{arg}"


@handle.register(int)
def _handle_int(arg):
    return f"int:{arg}"


print(handle(42))
print(handle("hi"))
print(handle(3.14))
print(handle([1, 2]))
