# corpus case — ground truth is CPython at run time (CHARTER I5).
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
