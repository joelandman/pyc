# corpus case — ground truth is CPython at run time (CHARTER I5).
class A:
    x = 10
    @classmethod
    def cm(cls):
        return cls.x
print(A.cm())
a = A()
print(a.cm())
