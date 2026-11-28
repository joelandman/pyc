# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: super() with explicit args, MRO across multiple inheritance.
class A:
    def greet(self):
        return "A"


class B(A):
    def greet(self):
        return "B>" + super().greet()


class C(A):
    def greet(self):
        return "C>" + super().greet()


class D(B, C):
    def greet(self):
        return "D>" + super().greet()


print(D().greet())
print(B().greet())


class Base:
    def __init__(self):
        self.base_set = True


class Mixin:
    def __init__(self):
        self.mixin_set = True


class Both(Base, Mixin):
    def __init__(self):
        super().__init__()


b = Both()
print(b.base_set, b.mixin_set)
