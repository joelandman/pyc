# corpus case — ground truth is CPython at run time (CHARTER I5).
def deco(f):
    def wrap(*a, **k):
        return f(*a, **k) + 1
    return wrap
@deco
def base(x):
    return x
print(base(5))
