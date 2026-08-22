# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    pass
class SpecificError(MyError):
    pass
try:
    raise SpecificError('specific')
except MyError as e:
    print('caught via ancestor:', e)
