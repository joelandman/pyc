# corpus case — ground truth is CPython at run time (CHARTER I5).
class Foo:
    def __repr__(self):
        return 'FooRepr'
foo = Foo()
print(f'{foo!r}')
