# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: match class patterns with keyword captures (uses __init__ sig).
class Pt:
    def __init__(self, x, y):
        self.x = x
        self.y = y


def describe(obj):
    match obj:
        case Pt(x=0, y=0):
            return "origin"
        case Pt(x=x, y=y):
            return f"({x},{y})"
        case _:
            return "not-a-point"


print(describe(Pt(0, 0)))
print(describe(Pt(3, 4)))
print(describe("str"))
