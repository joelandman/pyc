# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    del None[1:3]
    print('ok')
except TypeError as e:
    print(type(e).__name__)
