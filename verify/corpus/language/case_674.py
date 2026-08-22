# corpus case — ground truth is CPython at run time (CHARTER I5).
def outer():
    def inner(a):
        return a
    try:
        inner()
    except TypeError as e:
        print(e)
outer()
f = lambda a: a
try:
    f()
except TypeError as e:
    print(e)
