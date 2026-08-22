# corpus case — ground truth is CPython at run time (CHARTER I5).
class X:
    def hello(self):
        return "X.hello"
class Y(X):
    pass
class Z(Y):
    def hello(self):
        return "Z(" + super().hello() + ")"
print(Z().hello())
