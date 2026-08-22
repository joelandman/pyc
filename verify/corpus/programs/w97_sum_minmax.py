# W9.7 / I-170 I-173: sum(str) TypeError; empty min/max ValueError.
try:
    print(sum("ab"))
except TypeError as e:
    print(type(e).__name__)
print(sum(""))
print(sum((1, 2, 3)))
print(sum(b"ab"))
try:
    print(min([]))
except ValueError as e:
    print(type(e).__name__)
try:
    print(min(()))
except ValueError as e:
    print(type(e).__name__)
try:
    print(min(""))
except ValueError as e:
    print(type(e).__name__)
try:
    print(max([]))
except ValueError as e:
    print(type(e).__name__)
print(min([], default=99))
print(min([], default=None))
print(min([1, 2]))
print(max([3, 1]))
