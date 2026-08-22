# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    pass
try:
    try:
        raise MyError('wrong catch')
    except ValueError:
        print('should not print')
except MyError as e:
    print('correctly fell through:', e)
