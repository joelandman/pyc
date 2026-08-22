# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    print('{1}'.format('a'))
except IndexError as e:
    print(type(e).__name__)
try:
    print('{x}'.format())
except KeyError as e:
    print(type(e).__name__)
try:
    print('{} {}'.format(1))
except IndexError as e:
    print(type(e).__name__)
