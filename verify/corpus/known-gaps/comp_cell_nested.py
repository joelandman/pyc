# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A comprehension inside a comprehension: the inner one is two scopes away.
def outer():
    n = 5
    return [[n for _ in range(1)] for i in range(2)]


print(outer())
