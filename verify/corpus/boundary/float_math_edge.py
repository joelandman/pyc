import math
print(repr(math.floor(-0.5)), repr(math.ceil(-0.5)), repr(math.trunc(-0.5)))
print(repr(math.fmod(-7, 3)), repr(-7 % 3), repr(math.fmod(7, -3)))
print(repr(math.log(1e-300)), repr(math.log10(1e300)), repr(math.exp(709.7)))
for fn, arg in ((math.log, 0.0), (math.log, -1.0), (math.sqrt, -1.0),
                (math.exp, 1000.0), (math.asin, 2.0), (math.factorial, -1)):
    try:
        print(fn.__name__, arg, '->', repr(fn(arg)))
    except Exception as e:
        print(fn.__name__, arg, '->', type(e).__name__, e)
print(repr(math.hypot(3, 4)), repr(math.fsum([0.1] * 10)), repr(sum([0.1] * 10)))
print(repr(math.isclose(0.1 + 0.2, 0.3)), repr(math.ldexp(1.0, -1074)))
print(repr(math.frexp(0.0)), repr(math.frexp(-8.0)), repr(math.modf(-3.5)))
print(repr(math.gcd(2**70, 2**35)), repr(math.isqrt(10**40)))
print(repr(round(math.pi, 15)), repr(abs(-0.0)))
