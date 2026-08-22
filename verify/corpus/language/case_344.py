# corpus case — ground truth is CPython at run time (CHARTER I5).
def deco1(f):
    def wrap(*a, **k):
        return f(*a, **k) + 1
    return wrap
def deco2(f):
    def wrap(*a, **k):
        return f(*a, **k) * 2
    return wrap
@deco1
@deco2
def base(x):
    return x
print(base(5))
