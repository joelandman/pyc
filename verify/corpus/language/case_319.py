# corpus case — ground truth is CPython at run time (CHARTER I5).
def mixed(a, b, c):
    return a * 100 + b * 10 + c
print(mixed(1, **{'b': 2, 'c': 3}))
