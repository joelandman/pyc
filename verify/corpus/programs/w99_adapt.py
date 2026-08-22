# W9.9 / I-179 I-180 I-181: first-class min default/key; reversed(class); sum/sorted(None).
m = min
print(m([], default=99))
print(m([], default=None))
print(m([3, 1, 2], key=lambda x: -x))
print(m([1, 2]))
mx = max
print(mx([], default=0))
class C:
    pass
try:
    print(list(reversed(C)))
except TypeError as e:
    print(type(e).__name__)
print(list(reversed({1: 2})))
so = sorted
try:
    print(so(None))
except TypeError as e:
    print(type(e).__name__)
s = sum
try:
    print(s(None))
except TypeError as e:
    print(type(e).__name__)
print(so([3, 1, 2], reverse=True))
print(s((1, 2), 10))
