# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    def __init__(self, m):
        super().__init__(m)
try:
    raise MyError('boom')
except MyError as e:
    print(type(e).__name__ + ":", e)
