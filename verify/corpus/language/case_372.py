# corpus case — ground truth is CPython at run time (CHARTER I5).
class Circle:
    def __init__(self, r):
        self.r = r
    @property
    def area(self):
        return 3.14159 * self.r * self.r
circ = Circle(5)
print(circ.area)
