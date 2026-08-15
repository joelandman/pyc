# W8.5 / I-151 I-155: builtin * forms; * + **dict multiple values.
def mk():
    return [[1, 2, 3]]
print(min(*mk()))
def mz():
    return [[1, 2], [3, 4], [5, 6]]
print(list(zip(*mz())))
def g4(a, b=2):
    return (a, b)
def mk2():
    return [1, 9]
try:
    print(g4(*mk2(), **{"b": 3}))
except TypeError as e:
    print(type(e).__name__)
try:
    print(g4(*[1, 9], **{"b": 3}))
except TypeError as e:
    print(type(e).__name__)
def mkp():
    return [1, 2, 3]
print(*mkp())
