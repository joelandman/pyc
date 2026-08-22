# corpus case — ground truth is CPython at run time (CHARTER I5).
def app(f, xs):
    return f(*xs)
print(app(lambda *a: len(a), [1,2,3]))
