# corpus case — ground truth is CPython at run time (CHARTER I5).
print(list(zip([1, 2], [3, 4], [5, 6])))
print(list(zip([1, 2], [3, 4])))
print(list(zip((1, 2), (3, 4), (5, 6))))
print(list(zip([1, 2], (3, 4))))
print(list(zip(*[(1, 2), (3, 4), (5, 6)])))
def mt():
    return [(1, 2), (3, 4), (5, 6)]
print(list(zip(*mt())))
def mt2():
    return ((1, 2), (3, 4), (5, 6))
print(list(zip(*mt2())))
print(list(zip([1, 2, 3], [4, 5], [6, 7, 8, 9])))
print(list(zip([1], [2], [3], [4])))
