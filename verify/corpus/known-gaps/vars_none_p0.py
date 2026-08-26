def f():
    a = 1
    return vars() is None, dir() == []
print(f())
