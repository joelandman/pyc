import math
inf = float('inf')
ninf = float('-inf')
nan = float('nan')
print(repr(inf), repr(ninf), repr(nan))
print(repr(nan == nan), repr(nan != nan), repr(math.isnan(nan)))
print(repr(math.isinf(inf)), repr(math.isfinite(1.0)))
print(repr(inf - inf if False else math.isnan(inf - inf)))
print(repr(0.0), repr(-0.0), repr(0.0 == -0.0))
print(repr(math.copysign(1.0, -0.0)), repr(math.copysign(1.0, 0.0)))
print(repr(1 / -0.0 if False else math.copysign(math.inf, -0.0)))
try:
    print(repr(1 / 0.0))
except ZeroDivisionError as e:
    print('ZeroDivisionError', e)
print(repr(str(-0.0)), repr(math.atan2(-0.0, -1.0)))
print(repr(inf * 0.0 if False else math.isnan(inf * 0.0)))
print(repr(max(0.0, -0.0)), repr(min(-0.0, 0.0)))
print(repr(math.sqrt(2.0)), repr(math.pi), repr(math.e))
