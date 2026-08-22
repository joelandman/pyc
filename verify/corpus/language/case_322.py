# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    raise ValueError('boom')
except ValueError as e:
    print(type(e).__name__)
