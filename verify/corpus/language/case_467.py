# corpus case — ground truth is CPython at run time (CHARTER I5).
class A:
    def m(self):
        return "A"
class B(A):
    def m(self):
        return "B->" + super().m()
class C(A):
    def m(self):
        return "C->" + super().m()
class D(B, C):
    def m(self):
        return "D->" + super().m()
class E(B, C):
    pass
print(D().m())
print(E().m())
