# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(y):
    return y ** 2
print(f(3.5))
print(f(4))
print(f(-2.5))

def g(y):
    return y ** 3
print(g(2))
print(g(2.0))

def sq(x):
    return x * x
print(sq(3))
print(sq(3.5))

def cube(x):
    return x ** 4
print(cube(2))
print(cube(2.5))

def only_int(n):
    return n ** 2
print(only_int(5))
print(only_int(6))
