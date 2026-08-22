# corpus case — ground truth is CPython at run time (CHARTER I5).
a = [[1, 2], [3, 4]]
b = [10, 20]
print([x+y for row in a for x, y in [(row[0], row[1])]])
