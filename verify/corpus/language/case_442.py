# corpus case — ground truth is CPython at run time (CHARTER I5).
class O:
    pass
o = O()
o.x = 10
print(getattr(o, 'x'))
print(hasattr(o, 'x'))
print(hasattr(o, 'y'))
setattr(o, 'y', 20)
print(o.y)
