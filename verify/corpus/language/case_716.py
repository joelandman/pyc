def f(v):
    match v:
        case 0: return "zero"
        case x: return f"captured {x}"
print(f(0), f(5), f("s"))
def g(v):
    match v:
        case _: return "always"
print(g(1))
