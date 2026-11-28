# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: itertools recipes — chain, combinations_with_replacement,
# accumulate with initial, groupby edge, islice, starmap, zip_longest fill.
from itertools import (
    chain, combinations, combinations_with_replacement, accumulate,
    groupby, islice, starmap, zip_longest, product, permutations,
)

print(list(chain([1, 2], [3], (4, 5))))
print(list(combinations_with_replacement([1, 2, 3], 2)))
print(list(accumulate([1, 2, 3, 4], initial=0)))
print(list(starmap(lambda a, b: a + b, [(1, 2), (3, 4)])))
print(list(zip_longest([1, 2], [3], fillvalue=9)))
print(list(islice(range(10), 2, 8, 2)))
print(list(product([1, 2], repeat=2)))
print(list(permutations([1, 2, 3], 2)))

rows = [("a", 1), ("a", 2), ("b", 1)]
for k, g in groupby(rows, key=lambda r: r[0]):
    print(k, sorted(v for _, v in g))
