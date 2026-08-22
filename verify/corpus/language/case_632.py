# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    def __init__(self, m):
        super().__init__(m)
        self.extra = 7
e = MyError('x')
print(e, e.extra)
