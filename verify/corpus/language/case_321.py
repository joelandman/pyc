# corpus case — ground truth is CPython at run time (CHARTER I5).
def g(a=1, b=2, c=3):
    return a + b + c
print(g(**{}))
