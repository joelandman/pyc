# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# PEP 572: a walrus binds in the CONTAINING scope, and inside a comprehension
# that means the scope containing the comprehension. Two halves had to be
# fixed, and each failed differently:
#
#   the BINDING   -- the scope collector reached NamedExpr only as an
#                    assignment TARGET, so a walrus buried in an expression was
#                    never seen and the name was not a local at all;
#   the CAPTURE   -- the comprehension's captured set collected names that were
#                    READ, and a walrus WRITES, so the synthetic function wrote
#                    to a local slot of its own and the enclosing name stayed
#                    unassigned.
#
# A walrus in a LAMBDA binds in the lambda, not outward, and that is the
# control: it must keep NOT leaking.
def genexp():
    contains_one = any((last := num) == 1 for num in [1, 2, 3])
    return contains_one, last


def listcomp():
    total = 0
    partial = [total := total + v for v in range(5)]
    return partial, total


def dictcomp():
    d = {k: (doubled := k * 2) for k in range(3)}
    return d, doubled


def in_if():
    if (m := 10) > 5:
        return m


def in_while():
    while (n := 3) and False:
        pass
    return n


def in_lambda():
    f = lambda: (inner := 1)      # binds in the LAMBDA
    return f()


print("genexp:", genexp())
print("listcomp:", listcomp())
print("dictcomp:", dictcomp())
print("if:", in_if())
print("while:", in_while())
print("lambda:", in_lambda())
print("module:", [y := 5], y)
