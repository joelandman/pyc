# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    x = None
print('{0.x}'.format(C()))
class D:
    pass
d = D()
d.x = None
print('{0.x}'.format(d))
