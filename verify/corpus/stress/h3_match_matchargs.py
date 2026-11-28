# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: match class pattern with __match_args__ controlling positional
# captures, plus mapping pattern with **rest capture and nested mapping.
class Pt:
    __match_args__ = ("x", "y")


def describe(p):
    match p:
        case Pt(0, 0):
            return "origin"
        case Pt(x, y):
            return f"({x},{y})"
        case _:
            return "?"


print(describe(Pt(0, 0)))
print(describe(Pt(1, 2)))


def lookup(data):
    match data:
        case {"user": u, "meta": {"level": lvl}}:
            return (u, lvl)
        case {"user": u, **rest}:
            return (u, sorted(rest))
        case _:
            return None


print(lookup({"user": "a", "meta": {"level": 3}}))
print(lookup({"user": "b", "extra": 1}))
