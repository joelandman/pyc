# corpus case — ground truth is CPython at run time (CHARTER I5).
class Vec:
    def __init__(self, x, y):
        self.x, self.y = x, y
    def __bool__(self):
        return self.x != 0 or self.y != 0
print(bool(Vec(0, 0)), bool(Vec(1, 0)))
print('t' if Vec(0, 0) else 'f')
