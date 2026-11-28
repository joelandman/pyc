# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: unpacking assignment with splat on both ends.
first, *middle, last = [1, 2, 3, 4, 5]
print(first, middle, last)

a, *rest = [10, 20, 30]
print(a, rest)

*x, = [1]
print(x)

(p, q) = [(1, 2), (3, 4)]
print(p, q)
