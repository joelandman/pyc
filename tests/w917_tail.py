# W9.17 / I-204 I-205 I-206: GetSlice scalars; map/filter bytes; cmp_to_key leftovers.
n = 1
try:
    print(n[:])
except TypeError as e:
    print(type(e).__name__)
s = {1}
try:
    print(s[:])
except TypeError as e:
    print(type(e).__name__)
b = True
try:
    print(b[:])
except TypeError as e:
    print(type(e).__name__)
f = 1.0
try:
    print(f[:])
except TypeError as e:
    print(type(e).__name__)
print([1, 2, 3][1:])
print("abc"[1:])
print(list(map(str, b"ab")))
print(list(filter(None, b"ab")))
print(list(map(str, bytearray(b"ab"))))
print(list(filter(None, bytearray(b"ab"))))
print(list(map(str, [1, 2])))
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
k = cmp_to_key(cmp)
try:
    print(k(3, 4))
except TypeError as e:
    print(type(e).__name__)
try:
    print(k(3) < 1)
except TypeError as e:
    print(type(e).__name__)
print(sorted([3, 1, 2], key=k(3)))
print(k(3) < k(1))
print(k(1) < k(3))
print(sorted([3, 1, 2], key=k))
print(k(3).obj)
print(k(3)(1).obj)
