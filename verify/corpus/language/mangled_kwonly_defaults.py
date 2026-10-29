# Regression probe: a class-body parameter mangled by CPython must keep its
# keyword-only default. The kwdefaults dict is keyed by the PUBLISHED (mangled)
# spelling; keying it by the raw spelling made the trampoline's lookup miss,
# dropping the default and raising TypeError where CPython returns the value.
# (test_keywordonlyarg.py::test_mangling, test_positional_only_arg.py::test_mangling)

class Outer:
    def m(self):
        class X:
            def f(self, *, __a=42):
                return __a
        return X().f()
print(Outer().m())

class Top:
    g = lambda self, *, __a=7: __a
print(Top.g(None))

class Y:
    def h(self, *, __a=1, __b=2):
        return __a + __b
print(Y().h())
print(Y().h(_Y__b=10))
