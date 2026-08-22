# corpus case — ground truth is CPython at run time (CHARTER I5).
def add_to(s, v):
    s.add(v)
    return len(s)

def set_ops(a, b):
    return sorted(a.union(b)), sorted(a.intersection(b)), sorted(a.difference(b))

def set_pred(a, b):
    return a.issubset(b), a.issuperset(b)

def set_discard(s):
    s.discard(1)
    s.discard(2)
    return sorted(s)

def dict_pop(d, k):
    return d.pop(k), sorted(d.items())

def dict_setdefault(d):
    d.setdefault('new', 5)
    return sorted(d.items())

def dict_popitem_copy(d):
    c = d.copy()
    c.clear()
    return len(c), sorted(d.items())

st = {1, 2}
print(add_to(st, 3))
print(sorted(st))
print(set_ops({1, 2}, {2, 3}))
print(set_pred({1}, {1, 2}))
print(set_discard({1, 2, 3}))
print(dict_pop({'a': 1, 'z': 2}, 'z'))
print(dict_setdefault({'a': 1}))
print(dict_popitem_copy({'a': 1, 'b': 2}))
