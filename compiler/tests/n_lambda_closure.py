# corpus case — ground truth is CPython at run time (CHARTER I5).
def make_adder(n):
    return lambda x: x + n
add5 = make_adder(5)
print(add5(10))
