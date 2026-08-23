def k(**kw):
    return sorted(kw.items())
print(k())
print(k(x=1, y=2))
def m(a, *rest, **kw):
    return a, rest, sorted(kw.items())
print(m(1, 2, 3, z=9))
