# W11.2 / I-222 I-225: boxed file methods (list / mixed param); readline(n).
p = "/tmp/pyc_w112_io.txt"
with open(p, "w") as f:
    f.write("hello\nworld")

fs = [open(p)]
print(fs[0].read())
fs[0].close()
fs = [open(p)]
print(repr(fs[0].readline()))
print(repr(fs[0].readline()))
fs[0].close()
print([open(p)][0].readlines())

def gwrite(f, s):
    f.write(s)
gwrite([open(p, "w")][0], "xyz")
print(open(p).read())

with open(p, "w") as f:
    f.write("hello\nworld")
f = open(p)
print(repr(f.readline(2)))
print(repr(f.readline(10)))
f.close()
print(repr([open(p)][0].readline(2)))

class C:
    def read(self):
        return "user"
def g(x):
    return x.read()
print(g(C()))
print(g(open(p)))

def dread(d):
    return d.read()
try:
    dread({"a": 1})
except AttributeError:
    print("no-dict-read")
