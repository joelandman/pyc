# corpus case — ground truth is CPython at run time (CHARTER I5).
def f0(s):
    return s.upper(), s.lower()
def f1(s, x):
    return s.count(x), s.find(x)
def f2(s, a, b):
    return s.replace(a, b), s.center(5, b)
def f3(s, a, b, n):
    return s.replace(a, b, n)
def fl(l, x):
    l.append(x)
    return l.copy()
class C:
    def ping(self, x):
        return "user:" + str(x)
def fp(o, x):
    return o.ping(x)
print(f0("Ab"))
print(f1("banana", "a"))
print(f2("ab", "a", "-"))
print(f3("aaa", "a", "X", 2))
print(fl([1], 2))
print(fp(C(), 9))
