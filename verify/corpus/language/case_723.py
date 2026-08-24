def f(v):
    match v:
        case n if n > 100: return "big"
        case n if n > 10: return "medium"
        case n: return f"small {n}"
print(f(500), f(50), f(5))
def g(v):
    match v:
        case [a, b] if a == b: return "pair equal"
        case [a, b]: return f"pair {a},{b}"
        case _: return "no"
print(g([1,1]), g([1,2]), g([1]))
