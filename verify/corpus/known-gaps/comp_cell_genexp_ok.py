# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The control. A generator expression reads the SAME free variable correctly,
# because genexps are handed to CPython as marshalled code objects rather than
# lowered natively. That contrast is what localises the defect to the native
# comprehension path.
def outer():
    n = 5
    return list(n for i in range(2))


print(outer())
