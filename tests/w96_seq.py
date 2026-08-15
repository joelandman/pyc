# W9.6 / I-169 I-171: sum(bytes); SortedWithCmp walks tuple/str/bytes.
print(sum(b"ab"))
print(sum(bytearray(b"ab")))
print(sum(b"ab", 10))
print(sum((1, 2, 3)))
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
print(sorted((3, 1, 2), key=cmp_to_key(cmp)))
print(sorted("bac", key=cmp_to_key(cmp)))
print(sorted(b"bac", key=cmp_to_key(cmp)))
print(sorted([3, 1, 2], key=cmp_to_key(cmp)))
print(sorted((3, 1, 2), key=cmp_to_key(cmp), reverse=True))
