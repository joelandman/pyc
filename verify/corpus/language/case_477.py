# corpus case — ground truth is CPython at run time (CHARTER I5).
def shout(fn):
    def wrapper(x):
        return fn(x) + "!"
    return wrapper
@shout
def greet(name):
    return "hi " + name
print(greet("joe"))
@shout
@shout
def hey(name):
    return "hey " + name
print(hey("bob"))
def twice(fn):
    def wrapper(x):
        return fn(fn(x))
    return wrapper
@twice
def inc(n):
    return n + 1
print(inc(5))
