# corpus case — ground truth is CPython at run time (CHARTER I5).
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
print(sorted([3, 1, 2], key=cmp_to_key(cmp), reverse=True))
