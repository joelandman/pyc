# corpus case — ground truth is CPython at run time (CHARTER I5).
def foo():
    pass
def bar():
    pass
g = foo
print(foo is foo)
print(g is foo)
print(foo is bar)
print(foo == foo)
print(foo == bar)
def outer():
    x = 1
    def inner():
        return x
    return inner
f = outer()
print(str(f).startswith("<function"))
print(repr(f).startswith("<function"))
print(f())
