class Point:
    kind = "2d"
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def total(self):
        return self.x + self.y

p = Point(3, 4)
print(p.total())
print(p.kind)
print(Point.kind)
