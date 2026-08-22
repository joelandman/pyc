# W9.20 / I-212: set() of leftover non-iterables TypeError.
try:
    print(set(1 + 2j))
except TypeError as e:
    print(type(e).__name__)
def f():
    return 0
try:
    print(set(f))
except TypeError as e:
    print(type(e).__name__)
try:
    print(set(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(set(1))
except TypeError as e:
    print(type(e).__name__)
try:
    print(set(True))
except TypeError as e:
    print(type(e).__name__)
try:
    print(set(1.0))
except TypeError as e:
    print(type(e).__name__)
print(sorted(set([3, 1, 1, 2])))
print(sorted(set("ab")))
print(sorted(set({1: 2})))
print(set())
try:
    print(any(1 + 2j))
except TypeError as e:
    print(type(e).__name__)
