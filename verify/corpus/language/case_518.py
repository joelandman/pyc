# corpus case — ground truth is CPython at run time (CHARTER I5).
def f_copy(x):    return x.copy()
def f_clear(x):   x.clear(); return x
def f_append(l):  l.append(9); return l
def f_extend(l):  l.extend([7]); return l
def f_insert(l):  l.insert(0, 5); return l
def f_reverse(l): l.reverse(); return l
def f_keys(d):    return sorted(d.keys())
def f_items(d):   return sorted(d.items())
def f_setdef(d):  d.setdefault('n', 1); return sorted(d.items())
def f_popitem(d): d.popitem(); return len(d)
def f_upper(s):   return s.upper()
def f_strip(s):   return s.strip()
def f_center(s):  return s.center(6, "*")
def f_starts(s):  return s.startswith("a")
def f_rindex(s):  return s.rindex("a")

print(f_copy([1, 2]), f_copy({"a": 1}), f_copy({1, 2}))
print(f_clear([1, 2]), f_clear({"a": 1}))
print(f_append([1]), f_extend([1]), f_insert([1]), f_reverse([1, 2]))
print(f_keys({"b": 1, "a": 2}), f_items({"a": 1}))
print(f_setdef({"a": 1}), f_popitem({"a": 1}))
print(f_upper("hi"), f_strip("  x  "), f_center("ab"), f_starts("abc"), f_rindex("banana"))
print([1,2].copy(), {"a":1}.copy(), "hi".upper(), b"hi".upper())
