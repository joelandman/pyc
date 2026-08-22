# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    pass
try:
    raise MyError('custom')
except MyError as e:
    print(type(e).__name__)
