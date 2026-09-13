from typing import Generic

class C[T](list[T]):
    x = T
print(type(C.x).__name__)
print(C.__orig_bases__[0].__origin__.__name__)
print(C.__orig_bases__[1].__origin__.__name__)

class F[T, U: T]:
    pass
print(F.__type_params__[1].__bound__.__name__)

class G[T, U=T]:
    pass
print(G.__type_params__[1].__default__.__name__)

try:
    class E[T](Generic[T]):
        pass
    print('dup-ok')
except TypeError:
    print('dup')

def outer():
    T = 1
    class H[T](list[T]):
        y = T
    return T, type(H.y).__name__
print(outer())
