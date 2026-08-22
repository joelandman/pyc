# corpus case — ground truth is CPython at run time (CHARTER I5).
class MathUtils:
    @staticmethod
    def square(x):
        return x * x
print(MathUtils.square(4))
m = MathUtils()
print(m.square(4))
