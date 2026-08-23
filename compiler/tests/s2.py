def g(*args):
    return args
print(g())
print(g(1, 2, 3))
def h(a, *rest):
    return a, rest
print(h(1))
print(h(1, 2, 3))
