def f():
    pass
print(type(f.__code__).__name__)
import copy
def g(x):
    return x
print(copy.copy(g) is g)
print(copy.deepcopy(g) is g)
print(g.__defaults__ is None)
