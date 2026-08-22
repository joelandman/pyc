# corpus case — ground truth is CPython at run time (CHARTER I5).
import os
print(getattr(os, "missing", 99))
class C:
    pass
print(getattr(C(), "x", 7))
print(getattr(os, "path", "no") is not None)
o = C()
o.y = 3
print(getattr(o, "y", 8))
try:
    print(getattr(os, "missing"))
except AttributeError as e:
    print(type(e).__name__)
