class NoArgs: pass
class One:
    __match_args__ = ("a",)
    def __init__(self, a): self.a = a
class Missing:
    __match_args__ = ("nope",)

def f1():
    match One(1):
        case One(x, y): return "two"
        case _: return "fell"
def f2():
    match NoArgs():
        case NoArgs(z): return "bound"
        case _: return "fell"
def f3():
    match Missing():
        case Missing(q): return "bound"
        case _: return "fell"
def f4():
    match 5:
        case int(a, b): return "two"
        case _: return "fell"
def f5():
    match (1,2):
        case (a, b): return f"{a},{b}"
def f6():
    match [3, 9]:
        case [1 | 2 | 3, y]: return f"head ok, y={y}"
        case _: return "no"
def f7():
    match iter([1,2]):
        case [a, b]: return "seq"
        case _: return "not a sequence"
class D(dict): pass
def f8():
    match D(a=1):
        case {"a": v}: return f"a={v}"
        case _: return "no"
def f9():
    match {"x":1}:
        case {}: return "matches any mapping"
calls = []
def g(v):
    calls.append(v); return True
def f10():
    match 1:
        case n if g("first"): return str(calls)
def f11():
    match [1,[2,3]]:
        case [a, [b, c]]: return f"nested {a}{b}{c}"
def f12():
    match {"k": [1,2]}:
        case {"k": [x, y]}: return f"map-seq {x}{y}"

for label, fn in [("too many positional", f1), ("no __match_args__", f2),
                  ("attr missing", f3), ("int with 2 pos", f4),
                  ("tuple pattern", f5), ("nested or in seq", f6),
                  ("iterator not seq", f7), ("dict subclass", f8),
                  ("empty mapping", f9), ("guard side effects", f10),
                  ("nested sequence", f11), ("mapping of sequence", f12)]:
    try: print(f"{label}: {fn()}")
    except Exception as e: print(f"{label}: {type(e).__name__}: {e}")
