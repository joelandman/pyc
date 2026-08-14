# W6.3 / I-064 I-065: boxed **{} and dynamic *args defaults.
def f(a):
    return a
hs = [f]
try:
    print(repr(hs[0](**{})))
except TypeError as e:
    print(type(e).__name__)
def apply(fn):
    return fn(**{})
try:
    print(apply(f))
except TypeError as e:
    print(type(e).__name__)
print(repr(hs[0]({})))
def g(a, b=2):
    return (a, b)
def mk():
    return [1]
print(g(*mk()))
def h(a=1):
    return a
def empty():
    return []
print(h(*empty()))
