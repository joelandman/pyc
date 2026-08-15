# W9.8 / I-168 I-172 I-177: reversed(dict) keys; first-class adapters; sum(None).
print(list(reversed({1: 2})))
print(list(reversed({"a": 1, "b": 2})))
print(list(reversed({})))
print(list(reversed((1, 2, 3))))
s = sum
print(s((1, 2), 10))
print(s((1, 2, 3)))
so = sorted
print(so([3, 1, 2], reverse=True))
print(so([3, 1, 2]))
a = any
try:
    print(a(None))
except TypeError as e:
    print(type(e).__name__)
al = all
try:
    print(al(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(sum([1, None]))
except TypeError as e:
    print(type(e).__name__)
try:
    print(sum([None]))
except TypeError as e:
    print(type(e).__name__)
print(sum([1, 2, 3]))
