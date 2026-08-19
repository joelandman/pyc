# W11.1 / I-223 I-224 I-225 I-227: wb write(bytes), rb readlines,
# closed readlines, write-only read, open(Path).
from pathlib import Path
p = "/tmp/pyc_w111_io.txt"
with open(p, "wb") as f:
    f.write(b"hello\nworld")
print(open(p, "rb").read())
print(open(p, "rb").readlines())
f = open(p, "rb")
f.close()
try:
    print(f.readlines())
except ValueError:
    print("closed")
with open(p, "w") as f:
    try:
        f.write(b"xy")
        print("wrote-bytes-text")
    except TypeError:
        print("te-text")
with open(p, "wb") as f:
    try:
        f.write("hi")
        print("wrote-str-bin")
    except TypeError:
        print("te-bin")
    f.write(bytearray(b"AB"))
print(open(p, "rb").read())
f = open(p, "w")
try:
    print(repr(f.read()))
except Exception:
    print("not-readable")
f.close()
with open(p, "w") as f:
    f.write("pathok")
print(open(Path(p)).read())
