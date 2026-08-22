# corpus case — ground truth is CPython at run time (CHARTER I5).
print([0, 1, 2, 3, 4][-10::-1])
print("abcde"[-10::-1])
try:
    print([0, 1, 2, 3, 4][::0])
except ValueError as e:
    print(type(e).__name__)
h = [0, 1, 2, 3, 4]
try:
    h[-10::-1] = [9]
    print(h)
except ValueError as e:
    print(type(e).__name__)
h = [0, 1, 2, 3, 4]
try:
    h[::0] = []
    print(h)
except ValueError as e:
    print(type(e).__name__)
