# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    print('abc'.partition(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print('abc'.rpartition(1))
except TypeError as e:
    print(type(e).__name__)
try:
    print('abc'.partition(''))
except ValueError as e:
    print(type(e).__name__)
