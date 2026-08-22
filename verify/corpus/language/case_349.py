# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    pass
try:
    raise MyError('a', 'b')
except MyError as e:
    print(e.args[0], e.args[1])
