class X:
    global __class__
    __class__ = 42
    def f():
        __class__
print(globals()["__class__"])
print("__class__" in X.__dict__)
del globals()["__class__"]
