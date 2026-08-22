# corpus case — ground truth is CPython at run time (CHARTER I5).
print('{0[a:b]}'.format({'a:b': 1}))
print('{0[a!b]}'.format({'a!b': 1}))
try:
    print('{0[1]foo}'.format([0, 1]))
except ValueError as e:
    print(type(e).__name__)
