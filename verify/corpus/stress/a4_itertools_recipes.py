# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: itertools recipes — accumulate, islice, groupby, permutations.
from itertools import accumulate, islice, groupby, permutations


print(list(accumulate([1, 2, 3, 4])))
print(list(islice(range(10), 2, 6)))

rows = [("a", 1), ("a", 2), ("b", 3)]
for k, g in groupby(rows, key=lambda r: r[0]):
    print(k, [v for _, v in g])

print([p for p in permutations([1, 2, 3]) if p[0] == 1])
