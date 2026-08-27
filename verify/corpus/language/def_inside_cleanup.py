# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A nested function is a DIFFERENT function: the enclosing one's pending
# cleanups do not apply to it, and its blocks are not addressable from there.
# A `return` inside a def written in a try/finally or a `with` body used to be
# handed to the ENCLOSING function's cleanup and branch to its block, which
# emitted "use of undefined value '%bbN'" and failed to assemble the module.
class CM:
    def __enter__(self):
        print("enter")
        return self

    def __exit__(self, *a):
        print("exit")
        return False


with CM():
    def in_with():
        return "from a def inside with"


try:
    def in_finally():
        return "from a def inside try/finally"
finally:
    print("cleanup ran")


def outer_returns():
    with CM():
        def inner():
            return "inner"
        return inner() + " then outer"


print(in_with())
print(in_finally())
print(outer_returns())
