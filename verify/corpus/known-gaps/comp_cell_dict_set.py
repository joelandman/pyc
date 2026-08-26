# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# dict and set comprehensions share the implicit-function lowering.
def outer():
    n = 5
    return {i: n for i in range(2)}, sorted({n * i for i in range(3)})


print(outer())
