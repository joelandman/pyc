import contextlib

@contextlib.contextmanager
def f(arg: int) -> str:
    yield str(arg)

print(sorted(f.__annotations__))
print(f.__annotations__['arg'].__name__)
