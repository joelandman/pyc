# W9.11 / I-185 I-186: class iter leftovers; direct min 0-arg / default= + extras.
class C:
    pass
try:
    print(list(enumerate(C)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(zip(C, [1])))
except TypeError as e:
    print(type(e).__name__)
try:
    print(any(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(all(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(min(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(max(C))
except TypeError as e:
    print(type(e).__name__)
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
try:
    print(sorted(C, key=cmp_to_key(cmp)))
except TypeError as e:
    print(type(e).__name__)
print(list({1: 2}))
try:
    print(list(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(min())
except TypeError as e:
    print(type(e).__name__)
try:
    print(max())
except TypeError as e:
    print(type(e).__name__)
try:
    print(min(1, 2, default=0))
except TypeError as e:
    print(type(e).__name__)
try:
    print(min([1], foo=1))
except TypeError as e:
    print(type(e).__name__)
print(min([], default=99))
print(min(1, 2))
print(min([3, 1]))
