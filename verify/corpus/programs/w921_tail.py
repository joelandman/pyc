# W9.21 / I-213 I-211 plus fromkeys/deque/None-slice leftovers.
s = set
try:
    print(s(None))
except TypeError as e:
    print(type(e).__name__)
print(sorted(s([1, 2])))
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
k = cmp_to_key(cmp)
try:
    print(k == k(3))
except Exception as e:
    print(type(e).__name__)
try:
    print(k(3) == 3)
except TypeError as e:
    print(type(e).__name__)
print(k(3) < k(1))
print(sorted([3, 1, 2], key=k))
try:
    print(dict.fromkeys(1 + 2j))
except TypeError as e:
    print(type(e).__name__)
print(dict.fromkeys([1, 2], 0))
from collections import deque
try:
    print(list(deque(1 + 2j)))
except TypeError as e:
    print(type(e).__name__)
print(list(deque([1, 2])))
x = None
try:
    print(x[:])
except TypeError as e:
    print(type(e).__name__)
print([1, 2][1:])
try:
    print(set(None))
except TypeError as e:
    print(type(e).__name__)
