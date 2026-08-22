# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    pass
try:
    raise MyError('via Exception')
except Exception as e:
    print('caught via Exception:', e)
