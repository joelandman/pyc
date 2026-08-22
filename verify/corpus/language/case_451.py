# corpus case — ground truth is CPython at run time (CHARTER I5).
def make_add_ten():
    return lambda x: x + 10
add10 = make_add_ten()
print(add10(7))
