# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: a decorator using functools.wraps preserving name/doc.
from functools import wraps


def log(func):
    @wraps(func)
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)
    return wrapper


@log
def greet(name):
    "the docstring"
    return f"hi {name}"


print(greet.__name__)
print(greet.__doc__)
print(greet("world"))
