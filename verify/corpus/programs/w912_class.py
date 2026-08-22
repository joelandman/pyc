# W9.12 / I-187 I-189: class not a mapping for in/len; sum(C) is not iterable.
class C:
    pass
try:
    print("__mro__" in C)
except TypeError as e:
    print(type(e).__name__)
try:
    print(len(C))
except TypeError as e:
    print(type(e).__name__)
print(len({1: 2}))
print(1 in {1: 2})
try:
    print(sum(C))
except TypeError as e:
    print(e)
print(sum([1, 2, 3]))
try:
    print(list(C))
except TypeError as e:
    print(type(e).__name__)
