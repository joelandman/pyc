X = 1
print(type(globals()).__name__, 'X' in globals())
def f():
    return type(globals()).__name__
print(f())
print(type(locals()).__name__)
class C:
    z = 1
    t = type(locals()).__name__
print(C.t)
