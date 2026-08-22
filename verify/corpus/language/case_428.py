# corpus case — ground truth is CPython at run time (CHARTER I5).
def is_even(n):
    return n % 2 == 0
print(list(filter(is_even, [1, 2, 3, 4, 5, 6])))
