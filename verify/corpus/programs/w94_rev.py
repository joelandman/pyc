# W9.4 / I-163: reversed walks tuple/bytes; set/int TypeError not [] / setElems.
print(list(reversed([1, 2, 3])))
print(list(reversed((1, 2, 3))))
print(list(reversed("ab")))
print(list(reversed(b"ab")))
print(list(reversed(bytearray(b"ab"))))
try:
    print(list(reversed({1, 2})))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(reversed(1)))
except TypeError as e:
    print(type(e).__name__)
