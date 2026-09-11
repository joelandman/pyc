def f():
    x = 42
    class X:
        locals()["x"] = 43
        y = x
    class Y:
        locals()["x"] = 43
        del x
    return X.y, hasattr(Y, "x"), x
print(f())
