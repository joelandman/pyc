# The Computer Language Benchmarks Game -- pidigits (spigot algorithm).
#
# Perf workload (STATUS.md, real-world performance). Deterministic: the
# digit count is pinned below; prints the digits of pi. Big-integer
# arithmetic in pure Python -- the integer-heavy class. 1000000 digits
# (~0.3 s on the 3.14 oracle; CPython bigints are fast).
DIGITS = 1000000

def spigot(iterations):
    q, r, t, k, n, l = 1, 0, 1, 1, 3, 3
    for _ in range(iterations):
        q, r, t, k, n, l = ((q * 3 + r) * l,
                            (t * (7 * k) + 2) * q - l * (t * (7 * k) + 2 + r),
                            t,
                            k + 1,
                            (n * l * (7 * k) + l * (2 + n)) // (t * (7 * k) + 2 + 2 * r),
                            (l * (7 * k + 2)) // (t * (7 * k) + 2 + 2 * r))
        yield (q * n) // t - n

print("".join(str(d) for d in spigot(DIGITS)))
