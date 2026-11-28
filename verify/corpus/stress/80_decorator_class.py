# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: class decorator adding an attribute/method post-creation.
def add_answer(cls):
    cls.answer = 42
    return cls


@add_answer
class Service:
    def __init__(self):
        self.ready = True


s = Service()
print(s.answer)
print(s.ready)


def repeat(n):
    def decorator(func):
        def wrapper(*args, **kwargs):
            return [func(*args, **kwargs) for _ in range(n)]
        return wrapper
    return decorator


@repeat(3)
def boom():
    return "B"


print(boom())
