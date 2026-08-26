# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A parameter is a free variable to the comprehension just as a local is.
def outer(n):
    return [n * i for i in range(3)]


print(outer(5))
