# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: lru_cache memoization correctness and capacity eviction.
from functools import lru_cache

calls = []


@lru_cache(maxsize=2)
def fib(n):
    calls.append(n)
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print(fib(6))
print(fib(6))
print(sorted(calls))
print(fib.cache_size())
fib.cache_clear()
print(fib.cache_size())
