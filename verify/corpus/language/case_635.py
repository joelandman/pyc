# corpus case — ground truth is CPython at run time (CHARTER I5).
class A:
    def __init__(self, name):
        self.name = name
class B(A):
    def __init__(self, name):
        super().__init__(name)
print(B('rex').name)
