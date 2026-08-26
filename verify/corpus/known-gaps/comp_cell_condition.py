# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The `if` clause reads free variables too.
def outer():
    lim = 1
    return [i for i in range(4) if i > lim]


print(outer())
