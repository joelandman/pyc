# corpus case — ground truth is CPython at run time (CHARTER I5).
import os
def f(m):
    try:
        print(m.get("path"))
    except AttributeError as e:
        print(type(e).__name__)
f(os)
try:
    print(getattr(os, "get"))
except AttributeError as e:
    print(type(e).__name__)
class C129:
    def f(self):
        s = super()
        print(type(s).__name__)
        r = repr(s)
        print(r[:12] if len(r) >= 12 else r)
C129().f()
import re
print(repr(re.findall("a.b", "a\x00b")))
