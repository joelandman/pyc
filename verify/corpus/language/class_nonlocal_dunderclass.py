class T:
    def m(self):
        class X:
            nonlocal __class__
            __class__ = 42
            def f():
                __class__
        print(__class__)
T().m()
