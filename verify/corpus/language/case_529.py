# corpus case — ground truth is CPython at run time (CHARTER I5).
import functools
import operator

print(functools.reduce(lambda a, b: a + b, [1, 2, 3, 4]))
print(functools.reduce(operator.add, [1, 2, 3, 4], 10))

add5 = functools.partial(operator.add, 5)
print(add5(10))

def greet(greeting, name):
    return greeting + ", " + name + "!"
hello = functools.partial(greet, "Hello")
print(hello("World"))

@functools.wraps(greet)
def wrapper(a, b):
    return greet(a, b)
print(wrapper("Hi", "There"))

calls = []
@functools.lru_cache
def slow_square(x):
    calls.append(x)
    return x * x
print(slow_square(4))
print(slow_square(4))
print(slow_square(5))
print(calls)

@functools.lru_cache(maxsize=None)
def slow_cube(x):
    return x * x * x
print(slow_cube(3))
print(slow_cube(3))

print(operator.add(2, 3))
print(operator.sub(5, 2))
print(operator.mul(4, 3))
print(operator.truediv(10, 4))
print(operator.mod(10, 3))
print(operator.eq(1, 1))
print(operator.lt(2, 3))
print(operator.not_(True))
print(operator.neg(5))

pts = [{"x": 3}, {"x": 1}, {"x": 2}]
print(sorted(pts, key=operator.itemgetter("x")))
print(sorted([[3, "c"], [1, "a"], [2, "b"]], key=operator.itemgetter(0)))

f = operator.itemgetter(0, 2)
print(list(f([10, 20, 30, 40])))

def apply_fn(fn, x):
    return fn(x)
print(apply_fn(add5, 10))
getter = operator.itemgetter(1)
print(apply_fn(getter, [7, 8, 9]))
