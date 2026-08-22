# W7.1b / I-134: first-class named kwargs / non-empty **dict on boxed callee.
def f(a):
    return a
hs = [f]
print(hs[0](a=1))
print(hs[0](**{"a": 1}))
print(repr(hs[0]({"a": 1})))
def apply(fn):
    return fn(a=1)
print(apply(f))
try:
    print(repr(hs[0](**{})))
except TypeError as e:
    print(type(e).__name__)
print(repr(hs[0]({})))
def f2(a, b):
    return (a, b)
hs2 = [f2]
print(hs2[0](a=1, b=2))
print(hs2[0](**{"a": 1, "b": 2}))
try:
    print(hs[0](**{"a": 1, "x": 2}))
except TypeError as e:
    print(type(e).__name__)
