# corpus case — ground truth is CPython at run time (CHARTER I5).
def outer():
    n = 0
    f = lambda: n
    n = 42
    return f()
print(outer())
