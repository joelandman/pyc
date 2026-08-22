# corpus case — ground truth is CPython at run time (CHARTER I5).
h = [0, 1, 2, 3, 4]
del h[-10::-1]
print(h)
h = [0, 1, 2, 3, 4]
try:
    del h[::0]
    print(h)
except ValueError as e:
    print(type(e).__name__)
