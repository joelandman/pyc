# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def get(self, k, default=None):
        return "user-get:" + str(k)

c = C()
print(c.get("x"))
def f(o):
    return o.get("x")
print(f(c))
d = {"a": 1}
print(d.get("a"), d.get("z"), d.get("z", 9))
def g(x):
    return x.get("a"), x.get("z"), x.get("z", 9)
print(g(d))
