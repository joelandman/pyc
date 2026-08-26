# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# P0: str() of a cell succeeds, so the wrong value is printed silently.
def outer():
    n = 5
    return [str(n) for i in range(2)]


print(outer())
