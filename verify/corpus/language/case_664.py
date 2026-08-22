# corpus case — ground truth is CPython at run time (CHARTER I5).
import os
m = os
try:
    print(m.get("path"))
except AttributeError as e:
    print(type(e).__name__)
class C:
    def get(self, k, default=None):
        return "user-get:" + str(k)
c = C()
print(C.get(c, "x"))
import sys
try:
    print(sys.get("x"))
except AttributeError as e:
    print(type(e).__name__)
try:
    print(os.path.get("exists"))
except AttributeError as e:
    print(type(e).__name__)
from os import path
try:
    print(path.get("exists"))
except AttributeError as e:
    print(type(e).__name__)
q = os.path
try:
    print(q.get("exists"))
except AttributeError as e:
    print(type(e).__name__)
