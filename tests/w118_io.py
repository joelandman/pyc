# W10.1 / I-118: open().read / .readline / .close / "rb".
p = "/tmp/pyc_w118_io.txt"
with open(p, "w") as f:
    f.write("hello\nworld")
f = open(p, "r")
print(f.read())
f.close()
f = open(p)
print(repr(f.readline()))
print(repr(f.readline()))
print(repr(f.readline()))
f.close()
f = open(p, "r")
print(f.readlines())
f.close()
f = open(p, "r")
print(f.read(5))
f.close()
f = open(p, "rb")
print(f.read())
f.close()
f = open(p, "rb")
print(f.readline())
print(f.read())
f.close()
f = open(p, "r")
f.close()
try:
    print(f.read())
except ValueError as e:
    print(type(e).__name__)
with open(p, "r") as f:
    print(f.read())
