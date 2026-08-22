# corpus case — ground truth is CPython at run time (CHARTER I5).
def outer():
    a, b = 3, 4
    return (lambda: a + b)()
print(outer())
