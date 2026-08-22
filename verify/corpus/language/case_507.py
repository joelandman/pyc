# corpus case — ground truth is CPython at run time (CHARTER I5).
import itertools
import collections

print(itertools.chain([1, 2], [3, 4], [5]))
print(itertools.product([1, 2], [3, 4]))
print(itertools.combinations([1, 2, 3, 4], 2))
print(itertools.permutations([1, 2, 3]))
print(itertools.permutations([1, 2, 3], 2))
print(itertools.islice([1, 2, 3, 4, 5], 3))

def add(a, b):
    return a + b
print(itertools.starmap(add, [[1, 2], [3, 4], [5, 6]]))

print(itertools.zip_longest([1, 2, 3], [10, 20]))

c = collections.Counter(["a", "a", "a", "a", "b", "b", "c"])
print(collections.most_common(c))
print(collections.most_common(c, 2))
