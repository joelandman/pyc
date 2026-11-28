# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: reflected and duplicate operator overloading. The RHS __xxx__ must
# win when the LHS is a subclass. A silent-wrong here is a classic bug class.
class A:
    def __add__(self, other):
        return "A.__add__"

    def __radd__(self, other):
        return "A.__radd__"


class B(A):
    pass  # inherits both, but as a subclass B.__add__ should win in A + B


a = A()
b = B()
print(a + b)  # B is subclass of A -> B.__add__ wins (inherits A.__add__)
print(b + a)  # A.__radd__ wins? No: A.__add__ exists on A; reflected only if
# non-reflected returns NotImplemented. A.__add__ returns a str, so no reflection.

# duplicate operator: which __add__ fires (rightmost wins)
class Dup:
    def __add__(self, other):
        return "first"

    def __add__(self, other):
        return "second"


print(Dup() + Dup())
