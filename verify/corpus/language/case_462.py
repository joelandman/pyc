# corpus case — ground truth is CPython at run time (CHARTER I5).
def add(a, b):
    return a + b
def apply2(fn, x, y):
    return fn(x, y)
print(apply2(add, 2, 3))
ops = [add]
print(ops[0](10, 20))
d = {"a": add}
print(d["a"](1, 1))
def pick():
    return add
p = pick()
print(p(7, 8))
