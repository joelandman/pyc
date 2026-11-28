# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: match mapping pattern and @-attribute pattern.
def lookup(data):
    match data:
        case {"name": n, "age": a}:
            return (n, a)
        case _:
            return None


class Config:
    host = "h"
    port = 80


def report(obj):
    match obj:
        case Config(host=h):
            return f"host={h}"
        case _:
            return "?"


print(lookup({"name": "x", "age": 5}))
print(lookup({}))
print(report(Config()))
