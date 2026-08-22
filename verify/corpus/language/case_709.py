# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    pass
try:
    print(sum(C()))
except TypeError as e:
    print(e)
print(sum([1, 2, 3]))
try:
    print(C["__mro__"])
except TypeError as e:
    print(type(e).__name__)
try:
    print(C()["__class__"])
except TypeError as e:
    print(type(e).__name__)
class E:
    def __getitem__(self, k):
        return k
print(E()["x"])
print({1: 2}[1])
import sys
try:
    print(len(sys))
except TypeError as e:
    print(type(e).__name__)
try:
    print("argv" in sys)
except TypeError as e:
    print(type(e).__name__)
print(len({1: 2}))
print(1 in {1: 2})
try:
    print(len(C()))
except TypeError as e:
    print(type(e).__name__)
