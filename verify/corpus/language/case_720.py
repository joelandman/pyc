class P:
    __match_args__ = ("x", "y")
    def __init__(self, x, y): self.x, self.y = x, y
class Q: pass
for v in (P(1,2), P(0,9), Q(), 5, "s"):
    match v:
        case P(0, b): print("P with x=0, y=", b)
        case P(a, b): print("P", a, b)
        case Q(): print("a Q")
        case int(n): print("int", n)
        case str(s): print("str", s)
    print("  done")
