class Foo:
    def f(self_):
        global __x1
        __x1 = 0
        [_Foo__x1 := 1 for a in [2]]
        return __x1
print(Foo().f(), _Foo__x1)
