# corpus case — ground truth is CPython at run time (CHARTER I5).
def g(**kwargs):
    total = 0
    for k in kwargs:
        total += kwargs[k]
    return total
print(g(x=1, y=2, z=3))
print(g())
