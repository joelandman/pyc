# corpus case — ground truth is CPython at run time (CHARTER I5).
import itertools

print(list(itertools.accumulate([1, 2, 3, 4])))
print(list(itertools.accumulate([1, 2, 3, 4], lambda a, b: a * b)))

print(list(itertools.takewhile(lambda x: x < 5, [1, 3, 5, 2, 1])))
print(list(itertools.dropwhile(lambda x: x < 5, [1, 3, 5, 2, 1])))

print(list(itertools.compress(["a", "b", "c", "d"], [1, 0, 1, 0])))

data = [1, 1, 2, 2, 2, 3, 1, 1]
for k, g in itertools.groupby(data):
    print(k, list(g))

words = ["apple", "ant", "bear", "bee", "cat"]
for k, g in itertools.groupby(words, key=lambda w: w[0]):
    print(k, list(g))

print(list(itertools.chain.from_iterable([[1, 2], [3, 4], [5]])))

import itertools as it
print(list(it.chain.from_iterable([["a"], ["b", "c"]])))

from itertools import accumulate, takewhile, groupby
print(list(accumulate([5, 5, 5])))
print(list(takewhile(lambda x: x > 0, [3, 2, 1, -1, 5])))
for k, g in groupby(["x", "x", "y"], key=lambda w: w):
    print(k, list(g))

def wrap_groupby(data, keyfn):
    result = []
    for k, g in itertools.groupby(data, key=keyfn):
        result.append([k, list(g)])
    return result
print(wrap_groupby(["aa", "ab", "bc"], lambda w: w[0]))

def wrap_chain(nested):
    return list(itertools.chain.from_iterable(nested))
print(wrap_chain([[1, 2], [3, 4]]))
