# corpus case — ground truth is CPython at run time (CHARTER I5).
def outer():
    def inner(v):
        return v * 2
    a = inner
    b = inner
    print(a is b)
    return a
q = outer()
print(q(21))
