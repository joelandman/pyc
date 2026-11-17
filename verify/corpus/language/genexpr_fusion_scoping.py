# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Scoping semantics that the fusion MUST preserve when it lowers the genexpr
# inline instead of building a real generator:
#   * free variables are late-bound through cells (a rebind after the genexp
#     is created is seen later);
#   * the loop variable does not leak into the enclosing scope;
#   * nested for-clauses resolve in the right order;
#   * side effects are lazy -- nothing runs until the consumer advances;
#   * the outermost iterable is evaluated eagerly, and its exception surfaces
#     at creation, not at first next().
xs = [1, 2, 3]


def f():
    # free-var late binding through a fused consumer.
    gen = (i * n for i in xs)
    n = 100
    print(list(gen))

    # loop variable must not leak out of the genexpr.
    for _ in range(1):
        gathered = list(x for x in xs)
    print(gathered)
    print("x" not in dir())

    # nested for-clauses: inner runs fastest, order preserved.
    print([(i, j) for i in xs for j in xs if i != j])

    # side-effect laziness: the print inside the genexpr only fires as the
    # consumer advances -- any() short-circuits and must not run the rest.
    seen = []
    def g():
        for i in xs:
            seen.append(i)
            yield i
    print(any(v == 2 for v in g()))
    print(seen)  # must stop at 2 (short-circuit), not [1,2,3]

    # eager outer iterable: exception at creation, not first advance.
    def boom():
        raise RuntimeError("outer boom")
    try:
        sum(i for i in boom())
        print("no raise")
    except RuntimeError:
        print("caught outer boom")


f()
