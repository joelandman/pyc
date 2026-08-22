# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    pass
try:
    raise MyError('boom')
except MyError as e:
    print('caught:', e)
