def outer():
    T = int
    def inner(x: T) -> T:
        return x
    T = str
    return inner

print(outer().__annotations__["x"] is str)

def late():
    def inner(x: T):
        return x
    T = int
    return inner

print(late().__annotations__["x"] is int)

def cls():
    T = int
    class C:
        x: T
    return C

print(cls().__annotations__["x"] is int)
