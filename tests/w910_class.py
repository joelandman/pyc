# W9.10 / I-182 I-183 I-184: class not iterable; first-class min() / default= + extras.
class C:
    pass
try:
    print(list(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(tuple(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(sorted(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(set(C))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(C()))
except TypeError as e:
    print(type(e).__name__)
print(list({1: 2}))
try:
    print(list(reversed(C)))
except TypeError as e:
    print(type(e).__name__)
m = min
try:
    print(m())
except TypeError as e:
    print(type(e).__name__)
mx = max
try:
    print(mx())
except TypeError as e:
    print(type(e).__name__)
try:
    print(m(1, 2, default=0))
except TypeError as e:
    print(type(e).__name__)
try:
    print(m([1], foo=1))
except TypeError as e:
    print(type(e).__name__)
print(m([], default=99))
print(m([1, 2]))
