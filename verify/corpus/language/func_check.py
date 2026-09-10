import types, inspect

def f(x, y=1):
    return x + y

print(type(f) is types.FunctionType)
print(isinstance(f, types.FunctionType))
print(str(inspect.signature(f)))
print(f(2), f(2, 3))

def outer():
    n = 10
    def inner(x):
        return x + n
    n = 20
    return inner

g = outer()
print(g(1), g.__closure__[0].cell_contents)

class C:
    def m(self, x):
        return x
print(C().m(4), str(inspect.signature(C().m)))
