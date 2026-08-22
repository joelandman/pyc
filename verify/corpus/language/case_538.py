# corpus case — ground truth is CPython at run time (CHARTER I5).
class Foo:
    def __init__(self, n=5):
        self.n = n
    def show(self):
        print("n =", self.n)

X = Foo
y = X()
print(y.n)
y.show()

z = X(42)
print(z.n)

registry = {"foo": Foo}
w = registry["foo"](7)
print(w.n)

class Base:
    def __init__(self, v=1):
        self.v = v

class Child(Base):
    def __init__(self, v):
        super().__init__(v)

Y = Child
c = Y(99)
print(c.v)

def make(cls):
    return cls()
print(make(Foo).n)

class A:
    def __init__(self, n=1):
        self.n = n
class B:
    def __init__(self, n=2):
        self.n = n
print(A().n, B().n)

class Base2:
    def __init__(self, n=5):
        self.n = n

class Child2(Base2):
    def __init__(self):
        super().__init__()

print(Child2().n)
