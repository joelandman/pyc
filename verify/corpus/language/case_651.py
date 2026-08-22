# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    print('{0[999]}'.format([1, 2]))
except IndexError as e:
    print(type(e).__name__)
try:
    print('{0[k]}'.format({}))
except KeyError as e:
    print(type(e).__name__)
class C:
    pass
try:
    print('{0.missing}'.format(C()))
except AttributeError as e:
    print(type(e).__name__)
