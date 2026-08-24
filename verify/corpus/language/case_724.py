def a():
    match [1,2,3]:
        case [1, x, 99]: return "matched"
    try: return f"x={x}"
    except NameError: return "x unbound"
print(a())
def b():
    match 5:
        case n if n > 10: return "matched"
    try: return f"n={n}"
    except NameError: return "n unbound"
print(b())
