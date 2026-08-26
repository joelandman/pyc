# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# P0: the cell itself lands in the result list instead of its contents.
def outer():
    n = 5
    return [n for i in range(2)]


print(outer())
