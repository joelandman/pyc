# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A generator function is handed to CPython to compile, nested inside a
# synthetic wrapper so its free variables arrive as parameters. That wrapper
# used to change the meaning of the function's OWN name.
#
#     def powerset(U):
#         for S in powerset(U):     # at module level this is a GLOBAL
#             yield S
#
# symtable says the generator has no free variables, because the self-reference
# is a global. Nest the same def inside `_pyc_wrap` and `powerset` becomes a
# local of the wrapper, so the compiled generator captures it as a freevar
# instead, and the two disagreed:
#
#     'powerset' freevars ('powerset',) are not the same SET as symtable ()
#
# The shim is now renamed before compilation and its real name restored on the
# code object, so the self-reference resolves exactly as it did in the source:
# to the global when it was a global, and to the wrapper parameter when the
# name really is free -- a free name is already passed in as one.
def powerset(U):
    U = iter(U)
    try:
        x = frozenset([next(U)])
        for S in powerset(U):
            yield S
            yield S | x
    except StopIteration:
        yield frozenset()


def nested_recursive():
    def leaf(n):                      # genuinely free: captured, not global
        if n:
            yield from leaf(n - 1)
        yield n

    return list(leaf(3))


def name_is_restored():
    def g():
        yield 1

    return g.__name__, type(g()).__name__


print("module-level recursive:", sorted(len(s) for s in powerset([1, 2, 3])))
print("nested recursive:", nested_recursive())
print("name restored:", name_is_restored())
