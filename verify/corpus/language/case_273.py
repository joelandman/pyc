# corpus case — ground truth is CPython at run time (CHARTER I5).
def spaceship(a, b): return (a > b) - (a < b)
print(sorted([3, 1, 4, 1, 5], key=cmp_to_key(spaceship)))
