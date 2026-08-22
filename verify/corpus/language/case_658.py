# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    print('a b'.split(1))
except TypeError as e:
    print(type(e).__name__)
try:
    print('abc'.find(1))
except TypeError as e:
    print(type(e).__name__)
try:
    print('abc'.replace(1, 'x'))
except TypeError as e:
    print(type(e).__name__)
try:
    print('abc'.startswith(1))
except TypeError as e:
    print(type(e).__name__)
try:
    print(','.join([1, 2]))
except TypeError as e:
    print(type(e).__name__)
