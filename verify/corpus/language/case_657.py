# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    t = (5, 1, 8, 3)
    del t[1]
    print(t)
except TypeError as e:
    print(type(e).__name__)
try:
    s = "abcd"
    del s[1]
    print(s)
except TypeError as e:
    print(type(e).__name__)
try:
    t = (5, 1, 8, 3)
    t[1:3] = (9,)
    print(t)
except TypeError as e:
    print(type(e).__name__)
try:
    t = (5, 1, 8, 3)
    t[1] = 9
    print(t)
except TypeError as e:
    print(type(e).__name__)
ba = bytearray(b"abcd")
del ba[1:3]
print(ba)
ba = bytearray(b"abcd")
ba[1:3] = b"x"
print(ba)
