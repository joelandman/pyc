# W8.1 / I-142 I-146 I-147 I-148: dynamic * leftovers after W7.1.
def g(a, b):
    return (a, b)
def mk():
    return [1]
print(g(*mk(), b=3))
def g3(a, *args):
    return (a, len(args))
try:
    print(g3(*mk(), x=3))
except TypeError as e:
    print(type(e).__name__)
def g4(a, b=2):
    return (a, b)
try:
    print(g4(*[1, 9], b=3))
except TypeError as e:
    print(type(e).__name__)
def mk2():
    return [1, 9]
try:
    print(g4(*mk2(), b=3))
except TypeError as e:
    print(type(e).__name__)
def f(a, b):
    return (a, b)
print(f(*mk(), 2))
def a():
    return [1]
def b():
    return [2]
print(f(*a(), *b()))
def mkp():
    return [1, 2, 3]
print(*mkp())
