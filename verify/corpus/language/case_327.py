# corpus case — ground truth is CPython at run time (CHARTER I5).
class Vec:
    def __init__(self, x, y):
        self.x, self.y = x, y
v = Vec(1, 2)
print(v.x, v.y)
