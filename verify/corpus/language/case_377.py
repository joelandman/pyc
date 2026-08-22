# corpus case — ground truth is CPython at run time (CHARTER I5).
class A:
    def cube(self, x):
        return x ** 3
a = A()
print(a.cube(3))
