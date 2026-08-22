# W9.1 / I-156 I-157: static N-way zip; zip walks tuples not just lists.
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
