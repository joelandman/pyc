# corpus case — ground truth is CPython at run time (CHARTER I5).
def fs(s, x):
    s.add(x)
    return sorted(s.union({9}))
def fd(d, k):
    return d.get(k), d.get("z", 3)
def fi(n):
    return n.bit_length()
def fb(b):
    return b.upper()
print(fs({1, 2}, 3))
print(fd({"a": 1}, "a"))
print(fi(7), fi(True))
print(fb(b"ab"))
try:
    def bad(x):
        return x.nope()
    print(bad(1))
except AttributeError as e:
    print(type(e).__name__)
