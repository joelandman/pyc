# corpus case — ground truth is CPython at run time (CHARTER I5).
class MyError(Exception):
    pass
def risky():
    raise MyError('propagated')
try:
    risky()
except MyError as e:
    print('caught from function:', e)
