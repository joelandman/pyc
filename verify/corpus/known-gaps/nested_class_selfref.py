# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The same break, seen through the class's own name: `T` is a local of `f`, so
# a method referring to `T` is reading an enclosing function local. This is the
# shape behind test_abstract_numbers' `MyComplex` and test_bool's
# `SymbolicBool`, where a method constructs another instance of its own class.
def f():
    class T:
        def __init__(self, v):
            self.v = v

        def twin(self):
            return T(self.v * 2)

    return T(21).twin().v


print(f())
