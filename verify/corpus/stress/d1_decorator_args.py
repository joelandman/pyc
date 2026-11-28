# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: type alias, decorator with arguments, parametrized decorator.
from functools import wraps


type IntList = list[int]


print(IntList)


def repeat(times):
    def decorator(func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            results = []
            for _ in range(times):
                results.append(func(*args, **kwargs))
            return results
        return wrapper
    return decorator


@repeat(3)
def shout(word):
    return word.upper()


print(shout("hi"))
print(shout.__name__)
