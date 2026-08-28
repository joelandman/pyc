# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# concurrent.futures. Executor.map preserves INPUT order regardless of
# completion order, which is what makes it deterministic to compare.
from concurrent.futures import ThreadPoolExecutor, as_completed


def square(n):
    return n * n


def boom(n):
    if n == 3:
        raise ValueError("three")
    return n


with ThreadPoolExecutor(max_workers=4) as ex:
    mapped = list(ex.map(square, range(10)))

with ThreadPoolExecutor(max_workers=4) as ex:
    futures = [ex.submit(square, i) for i in range(10)]
    completed = sorted(f.result() for f in as_completed(futures))

with ThreadPoolExecutor(max_workers=2) as ex:
    futures = [ex.submit(boom, i) for i in range(5)]
    errors = sorted(type(f.exception()).__name__ for f in futures if f.exception())
    values = sorted(f.result() for f in futures if not f.exception())

print("map preserves order:", mapped)
print("as_completed sorted:", completed)
print("exception surfaced:", errors)
print("other results:", values)
