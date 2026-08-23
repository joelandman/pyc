mul = 100
def f(items):
    s = 2
    return (x * s * mul for x in items)
print(list(f([1, 2, 3])))
def late():
    n = 1
    g = (x * n for x in [1, 2])
    n = 10
    return list(g)
print("late binding:", late())
def src(tag):
    print("  evaluated", tag)
    return [1, 2]
h = (x for x in src("outer"))
print("created; outer already ran above")
print("  ->", list(h))
print("nested:", list((x + y for x in [1, 2] for y in [10, 20])))
print("filtered:", list(x for x in range(10) if x % 3 == 0))
import inspect
gg = (x for x in [1])
print("type:", type(gg).__name__, "| isgenerator:", inspect.isgenerator(gg))
print("sum:", sum(x * x for x in range(5)))
print("join:", ",".join(str(x) for x in range(4)))
import itertools
inf = (x for x in itertools.count())
print("infinite, taking 3:", [next(inf) for _ in range(3)])
try:
    bad = (x for x in (1 // 0,))
except ZeroDivisionError as e:
    print("eager outer raises at creation:", e)
