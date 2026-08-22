# corpus case — ground truth is CPython at run time (CHARTER I5).
def uses_get(**kwargs):
    return kwargs.get('missing', 'default')
print(uses_get(a=1))
