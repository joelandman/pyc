def counter(start, step=1):
    n = start
    while True:
        yield n
        n += step
c = counter(10, 5)
print([next(c) for _ in range(4)])
def make(mult):
    def scaled(xs):
        for x in xs:
            yield x * mult
    return scaled
print(list(make(3)([1,2,3])))
def echo():
    while True:
        got = yield
        if got is None: break
        print("  got", got)
e = echo(); next(e); e.send("a"); e.send("b")
def closes():
    try:
        yield 1
    finally:
        print("  cleaned up")
z = closes(); next(z); z.close()
def thrower():
    try:
        yield 1
    except ValueError:
        yield "caught"
t = thrower(); next(t); print("throw ->", t.throw(ValueError("x")))
class C:
    def items(self):
        yield "m1"
        yield "m2"
print(list(C().items()))
def outer_gen():
    yield from (x*2 for x in [1,2])
    yield 99
print(list(outer_gen()))
import inspect
print("isgeneratorfunction:", inspect.isgeneratorfunction(counter))
print("name/qualname:", counter.__name__, "|", make(1).__qualname__)
