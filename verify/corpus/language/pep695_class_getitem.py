class C[T]:
    pass
print(C[int])
print([c.__name__ for c in C.__mro__])
print(C.__orig_bases__[0].__origin__.__name__)

class B:
    pass
class D[T](B):
    pass
print([c.__name__ for c in D.__mro__])
print(D[int])

class Base:
    def __init_subclass__(cls):
        pass
class Sub[U](Base):
    pass
try:
    Sub[int]
except AttributeError:
    print('attr')
except TypeError:
    print('type')
