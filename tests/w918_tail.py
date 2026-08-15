# W9.18 / I-208 I-209: map/filter of complex/function; k(3)==3 TypeError.
try:
    print(list(map(str, 1 + 2j)))
except TypeError as e:
    print(type(e).__name__)
def f():
    return 0
try:
    print(list(map(str, f)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(filter(None, 1 + 2j)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(filter(None, f)))
except TypeError as e:
    print(type(e).__name__)
print(list(map(str, [1, 2])))
print(list(map(str, b"ab")))
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
k = cmp_to_key(cmp)
try:
    print(k(3) == 3)
except TypeError as e:
    print(type(e).__name__)
try:
    print(k(3) != 3)
except TypeError as e:
    print(type(e).__name__)
try:
    print(k(3) < 1)
except TypeError as e:
    print(type(e).__name__)
print(k(3) < k(1))
print(k(1) == k(1))
print(sorted([3, 1, 2], key=k))
