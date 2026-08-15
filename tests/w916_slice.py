# W9.16 / I-201 I-202 I-203: GetSlice; map/filter of scalars; cmp_to_key callable.
class C:
    pass
try:
    print(C[:])
except TypeError as e:
    print(type(e).__name__)
try:
    print(C()[:])
except TypeError as e:
    print(type(e).__name__)
import sys
try:
    print(sys[:])
except TypeError as e:
    print(type(e).__name__)
try:
    print({1: 2}[:])
except KeyError as e:
    print(type(e).__name__)
print([1, 2, 3][1:])
print((1, 2, 3)[1:])
print("abc"[1:])
try:
    print(list(map(str, None)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(map(str, True)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(map(str, 1)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(filter(None, 1.0)))
except TypeError as e:
    print(type(e).__name__)
print(list(map(str, [1, 2])))
print(list(filter(None, [0, 1, 2])))
print(list(map(str, {1: 2})))
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
k = cmp_to_key(cmp)
print(k(3) < k(1))
print(k(1) < k(3))
print(k(3) > k(1))
print(k(1) == k(1))
print(sorted([3, 1, 2], key=k))
print(k(3).obj)
