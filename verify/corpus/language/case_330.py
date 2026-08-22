# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    pass
c = C()
c.x, y, c.z = 1, 2, 3
print(c.x, y, c.z)
