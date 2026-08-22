# W7.2 / I-138: os.path / pathlib keep embedded NUL.
import os
from pathlib import Path
print(repr(os.path.basename("a\x00b")))
print(repr(os.path.dirname("x/a\x00b")))
print(repr(os.path.split("a\x00b/c")[1]))
print(repr(os.path.splitext("a\x00b.txt")[0]))
print(repr(os.path.basename("a/b\x00c")))
print(repr(os.path.join("a\x00b", "c")))
print(Path("ab").name)
