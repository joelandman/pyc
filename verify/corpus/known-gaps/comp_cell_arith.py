# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The same defect, but loud: arithmetic on a cell raises TypeError.
def outer():
    n = 5
    return [n * i for i in range(3)]


print(outer())
