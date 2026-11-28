# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: reduce with/without initializer, partial application in reduce.
from functools import reduce
from operator import add, mul


print(reduce(lambda a, b: a + b, [1, 2, 3, 4]))
print(reduce(add, [1, 2, 3, 4]))
print(reduce(mul, [1, 2, 3, 4]))
print(reduce(add, [], 100))
print(reduce(lambda a, b: a * b, [1, 2, 3, 4], 10))
