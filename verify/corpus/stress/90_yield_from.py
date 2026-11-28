# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: yield from delegating to sub-generators and the return value.
def count(start, stop):
    while start < stop:
        yield start
        start += 1


def flatten(*ranges):
    for r in ranges:
        yield from count(0, r)


print(list(flatten(3, 2, 4)))


def inner():
    yield 1
    yield 2
    return "done"


def outer():
    result = yield from inner()
    yield result


print(list(outer()))
