# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    pass
try:
    print(len(C()))
except TypeError as e:
    print(type(e).__name__)
try:
    print("__class__" in C())
except TypeError as e:
    print(type(e).__name__)
try:
    print(1 in C())
except TypeError as e:
    print(type(e).__name__)
class D:
    def __len__(self):
        return 5
    def __contains__(self, x):
        return x == 1
print(len(D()))
print(1 in D())
print(2 in D())
print(len({1: 2}))
print(1 in {1: 2})
try:
    print(len(C))
except TypeError as e:
    print(type(e).__name__)
