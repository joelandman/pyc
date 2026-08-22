# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    print('abc'.partition(''))
except ValueError as e:
    print(type(e).__name__ + ":", e)
try:
    print('abc'.rpartition(''))
except ValueError as e:
    print(type(e).__name__ + ":", e)
print('abc'.partition('b')[0])
