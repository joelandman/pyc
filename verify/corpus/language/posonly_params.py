# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# `def f(a, b, /, c, *, e)`. The markers are carried entirely by two counts:
# the trampoline binds positionally from slot 0 and BY KEYWORD from nposonly,
# so `/` is "keyword binding starts later" and `*` is "positional binding stops
# earlier". No separate machinery.
#
# The rule that reading the grammar would miss: a keyword matching a
# positional-only name is NOT an error when the function has **kwargs -- it
# lands there. Which is why `def g(a, /, **kw)` can be called as `g(1, a=2)`.
def f(a, b, /, c, d=4, *, e=5):
    return (a, b, c, d, e)


print("normal:", f(1, 2, 3))
print("kw for the rest:", f(1, 2, c=30, d=40, e=50))
try:
    f(a=1, b=2, c=3)
except TypeError as t:
    print("posonly by keyword ->", t)


def g(a, /, **kw):
    return a, kw


print("shadowed in **kw:", g(1, a=2))


def h(x, /, y, **kw):
    return x, y, kw


print("mixed:", h(1, 2, x=9, z=3))


def d(a, b=2, /, c=3):
    return a, b, c


print("posonly defaults:", d(1), d(1, 20), d(1, 20, 30))


def allpo(a, b, /):
    return a + b


print("all posonly:", allpo(1, 2))


def star(a, /, *rest):
    return a, rest


print("with *args:", star(1, 2, 3))

lam = lambda a, /, b: (a, b)
print("lambda:", lam(1, 2))
try:
    lam(a=1, b=2)
except TypeError as t:
    print("lambda posonly by keyword ->", t)
