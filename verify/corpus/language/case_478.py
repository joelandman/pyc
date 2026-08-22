# corpus case — ground truth is CPython at run time (CHARTER I5).
def repeat(n):
    def deco(fn):
        def wrapper(x):
            out = []
            for _ in range(n):
                out.append(fn(x))
            return out
        return wrapper
    return deco
@repeat(3)
def salute(name):
    return "yo " + name
print(salute("ann"))
def log(fn):
    def wrapper(a, b):
        r = fn(a, b)
        print("call ->", r)
        return r
    return wrapper
@log
def add(a, b):
    return a + b
print(add(2, 3))
g = add
print(g(10, 20))
def hof(f, x, y):
    return f(x, y)
print(hof(add, 1, 1))
