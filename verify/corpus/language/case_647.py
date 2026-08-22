# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    t = (5, 1, 8, 3)
    del t[1:3]
    print(t)
except TypeError as e:
    print(type(e).__name__)
try:
    s = "abcd"
    del s[1:3]
    print(s)
except TypeError as e:
    print(type(e).__name__)
