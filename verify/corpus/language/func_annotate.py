def f(x: int, y: str = "a") -> list:
    return [x, y]

print(sorted(f.__annotations__))
print(f.__annotations__["x"] is int, f.__annotations__["y"] is str)
print(f.__annotations__["return"] is list)
print(f.__annotate__ is not None)

def bare():
    pass
print(bare.__annotations__, bare.__annotate__)

class C:
    def m(self, x: int) -> None:
        pass
print(C.m.__annotations__["x"] is int)

def outer():
    def inner(x: int) -> int:
        return x
    return inner
print(outer().__annotations__["x"] is int)
