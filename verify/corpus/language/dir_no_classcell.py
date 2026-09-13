class T:
    def test_dir(self):
        junk = 12
        print(sorted(dir()))
        class C:
            def m(self):
                super()
            def __getclass(self):
                return type(self)
            __class__ = property(__getclass)
T().test_dir()
