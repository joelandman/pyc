# corpus case — ground truth is CPython at run time (CHARTER I5).
data = [1, 2, 3, 4]
print([y for x in data if (y := x*2) > 2])
