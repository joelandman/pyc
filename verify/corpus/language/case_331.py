# corpus case — ground truth is CPython at run time (CHARTER I5).
class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y
    def __eq__(self, other):
        return self.x == other.x and self.y == other.y
p1 = Point(1, 2)
print(p1 == Point(1, 2))
print(p1 == Point(9, 9))
print(p1 != Point(9, 9))
