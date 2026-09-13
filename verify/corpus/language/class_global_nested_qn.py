def f():
    global Y
    class Y:
        class Inside:
            pass
        def meth(self):
            pass
    return Y.__qualname__, Y.Inside.__qualname__, Y.meth.__qualname__
print(f())
