# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# P0: a free variable read inside a comprehension yields the raw cell, and a
# cell object is always truthy. Exit 0, wrong answer, no diagnostic.
def outer():
    flag = False
    return [bool(flag) for i in range(2)]


print(outer())
