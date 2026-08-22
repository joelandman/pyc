# corpus case — ground truth is CPython at run time (CHARTER I5).
def outer():
    n = 5
    f = lambda x: x + n
    return f(10)
print(outer())
