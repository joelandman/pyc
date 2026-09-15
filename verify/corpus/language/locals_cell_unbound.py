def f():
    def inner():
        return x
    print("unbound", "x" in locals())
    x = 7
    v = locals()["x"]
    print("bound", type(v).__name__, v)

f()
