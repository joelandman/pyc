# corpus case — ground truth is CPython at run time (CHARTER I5).
class Foo:
    def __init__(self):
        self.c5 = 'attr'
        self.t9 = 99
foo = Foo()
print(foo.c5, foo.t9)
