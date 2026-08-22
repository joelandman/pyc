# corpus case — ground truth is CPython at run time (CHARTER I5).
from collections import Counter

m = Counter({'a': 10, 'b': 3})
print(m['a'], m['b'])
print(sorted(m.keys()))
print(len(m))

c2 = Counter(m)
print(c2['a'], c2['b'])

print(sorted(Counter('aab').items()))
print(Counter({'x': 5}).most_common(1))
print(sorted(Counter(['a', 'a', 'b']).items()))
