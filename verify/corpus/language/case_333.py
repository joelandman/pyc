# corpus case — ground truth is CPython at run time (CHARTER I5).
class Vec:
    def __init__(self, x, y):
        self.x, self.y = x, y
    def __lt__(self, other):
        return (self.x, self.y) < (other.x, other.y)
    def __sub__(self, other):
        return Vec(self.x - other.x, self.y - other.y)
    def __mul__(self, scalar):
        return Vec(self.x * scalar, self.y * scalar)
    def __neg__(self):
        return Vec(-self.x, -self.y)
    def __len__(self):
        return 2
    def __repr__(self):
        return f'Vec({self.x},{self.y})'
v1 = Vec(1, 2)
v2 = Vec(3, 4)
print(v1 < v2)
print(v1 - v2)
print(v1 * 3)
print(-v1)
print(len(v1))
