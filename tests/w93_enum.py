# W9.3 / I-162: enumerate walks tuple/str/bytes; list + start= kept.
print(list(enumerate([10, 20])))
print(list(enumerate((1, 2))))
print(list(enumerate("ab")))
print(list(enumerate(b"ab")))
print(list(enumerate([1, 2], start=3)))
print(list(enumerate("ab", 5)))
try:
    print(list(enumerate(None)))
except TypeError as e:
    print(type(e).__name__)
