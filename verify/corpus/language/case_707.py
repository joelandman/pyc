# corpus case — ground truth is CPython at run time (CHARTER I5).
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
