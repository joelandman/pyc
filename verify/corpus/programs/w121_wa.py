# W12.1 / I-217 I-218 I-219
from collections import deque
print(list(deque()))
try:
    deque(None)
except TypeError:
    print("deque-none")
try:
    print({1} | None)
except TypeError:
    print("or-none")
try:
    print({1} & None)
except TypeError:
    print("and-none")
try:
    print({1} - None)
except TypeError:
    print("sub-none")
print({1} | {2})
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
k = cmp_to_key(cmp)
try:
    print(k == k)
except AttributeError:
    print("factory-eq")
print(k(3) < k(1))
