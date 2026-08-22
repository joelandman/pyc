# corpus case — ground truth is CPython at run time (CHARTER I5).
import os
print(repr(os.path.basename("a\x00b")))
print(repr(os.path.dirname("x/a\x00b")))
print(repr(os.path.split("a\x00b/c")[1]))
print(repr(os.path.splitext("a\x00b.txt")[0]))
print(repr(os.path.basename("a/b\x00c")))
print(repr(os.path.join("a\x00b", "c")))
