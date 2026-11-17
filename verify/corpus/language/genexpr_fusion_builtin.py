# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Draining consumers over a generator expression. This is the perf-fusion
# shape: the genexpr is consumed once by a loop that drains it, so no
# generator object needs to survive. Every builtin here must agree with
# CPython exactly -- the fusion lowers the genexpr inline, so a wrong
# accumulator, a missed short-circuit or a reordered reduction is a
# STDOUT_DIFFERS.
n = 5
xs = list(range(n))


def f():
    a = 10
    # sum over a single-clause genexpr with a free variable.
    print(sum(i for i in xs))
    print(sum(i * a for i in xs))
    # min/max over a genexpr.
    print(min(i * i for i in xs))
    print(max(i * i for i in xs))
    # any/all short-circuit correctly. (len() over a generator expression is
    # NOT valid in 3.14 -- it raises TypeError on both pyc and the oracle, so
    # it is not a consumer here.)
    print(any(i > 3 for i in xs))
    print(all(i >= 0 for i in xs))
    print(all(i > 0 for i in xs))  # empty-ish guard -> all() on empty is True
    # materialisers.
    print(list(i * 2 for i in xs))
    print(tuple(-i for i in xs))
    print(set(i % 3 for i in xs))
    # sorted by a key.
    print(sorted(xs, key=lambda z: -z))
    print(sorted((i * i for i in xs)))
    # str.join over a genexpr.
    print(",".join(str(i) for i in xs))
    # nested genexprs fused in one expression.
    print(sum(sum(j for j in range(i)) for i in xs))
    # a genexpr whose outermost iterable is itself a comprehension.
    print(sum(i for row in [xs, xs] for i in row))


f()
