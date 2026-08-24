class P:
    __match_args__ = ("x",)
    def __init__(self, x, y): self.x, self.y = x, y
match P(1, 2):
    case P(x=a, y=b): print("kw", a, b)
match P(1, 2):
    case P(a, y=b): print("mixed", a, b)
