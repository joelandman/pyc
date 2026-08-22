# W12.4 / I-158: nested generator wrap; list(int) does not drain yields.
def outer():
    def inner():
        yield 1
        yield 2
    return list(inner())
print(outer())

def g():
    yield 7
    try:
        print(list(1))
    except TypeError:
        print("te")
print(list(g()))
