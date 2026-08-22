# corpus case — ground truth is CPython at run time (CHARTER I5).
a, b = (lambda x: x-1), (lambda x: x+1)
print(a(10), b(10))
