# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: supported match features — guards, or-patterns, *rest, kw captures.
def kind(value):
    match value:
        case 0 | 1 | 2:
            return "small"
        case v if isinstance(v, int) and v > 100:
            return f"big:{v}"
        case str() | bytes():
            return "text"
        case [1, *rest]:
            return f"list1:{rest}"
        case _:
            return "other"


print(kind(0))
print(kind(150))
print(kind("x"))
print(kind([1, 2, 3]))


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y


def where(p):
    match p:
        case Point(x=0):
            return "on-y-axis"
        case Point(x=x, y=y):
            return f"({x},{y})"


print(where(Point(0, 5)))
print(where(Point(3, 4)))
