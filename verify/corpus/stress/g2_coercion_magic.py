# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: coercion magic methods: __index__, __float__, __bool__, __int__,
# __format__. These change how a value flows into other operations and into
# format specs; a silent-wrong is easy to introduce.
class Count:
    def __init__(self, n):
        self.n = n

    def __index__(self):
        return self.n

    def __float__(self):
        return float(self.n) * 2.0

    def __bool__(self):
        return self.n != 0


print([0, 1, 2][Count(1)])
print(range(Count(3)))
print(float(Count(2.5)))
print(bool(Count(0)), bool(Count(7)))

# __format__ driving a format spec
class Money:
    def __format__(self, spec):
        return f"{spec}:M"


print(format(Money(), "x"))
print(f"{Money():x}")
