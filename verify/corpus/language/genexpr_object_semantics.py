# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A generator expression that is EXPOSED as a real generator object (not
# fused into a draining consumer). pyc runs these through a real CPython
# generator, so every observable generator property must agree: object
# identity, send/throw/stop, gi_frame, __name__, and late-bound free vars.
#
# This is the differential gate for any future attempt to lower a genexp
# natively as an exposed object: it must reproduce all of this or it is a
# different object (GENERATORS.md, "Semantics that must be reproduced").
xs = [1, 2, 3, 4]


def f():
    # object identity.
    g = (i * 2 for i in xs)
    import types
    print(types.isgenerator(g))
    print(type(g).__name__)
    print(g.__name__ == "<genexpr>")
    print(hasattr(g, "gi_frame"))

    # next() advances; StopIteration ends it.
    vals = []
    vals.append(next(g))
    vals.append(next(g))
    vals.append(next(g))
    print(vals)
    try:
        next(g)
        print("no stop")
    except StopIteration:
        print("stop")

    # send() drives it (PEP 380 resume).
    def g2():
        seen = []
        while True:
            x = yield len(seen)
            seen.append(x)
    s = g2()
    print(next(s))
    print(s.send("a"))
    print(s.send("b"))

    # throw() propagates into the suspended genexp.
    def g3():
        try:
            yield 1
        except ValueError:
            yield "caught"
    t = g3()
    next(t)
    print(t.throw(ValueError()))

    # free variables are late-bound through cells.
    n = 1
    late = (i * n for i in xs)
    n = 10
    print(list(late))


f()
