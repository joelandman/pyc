# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    print([1, 2, 3][-10])
except IndexError as e:
    print('caught:', e)
