# W11.3 / I-119: hashlib .update() incremental.
import hashlib
h = hashlib.md5()
h.update(b"hello")
h.update(b" world")
print(h.hexdigest())
print(hashlib.md5(b"hello world").hexdigest())
h2 = hashlib.sha1(b"ab")
h2.update(b"c")
print(h2.hexdigest())
print(hashlib.sha1(b"abc").hexdigest())
h3 = hashlib.sha256()
h3.update(b"x")
print(h3.hexdigest())
print(h3.digest())
h4 = hashlib.md5()
hs = [h4]
hs[0].update(b"hi")
print(hs[0].hexdigest())
print(hashlib.md5(b"hi").hexdigest())
class C:
    def update(self, x):
        return "user"
print(C().update(1))
def gu(x, v):
    x.update(v)
d = {}
gu(d, {"a": 1})
print(d["a"])
hz = hashlib.md5()
gu(hz, b"z")
print(hz.hexdigest())
print(hashlib.md5(b"z").hexdigest())
