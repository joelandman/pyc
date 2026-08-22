# corpus case — ground truth is CPython at run time (CHARTER I5).
from collections import Counter

c = Counter('aab')
c.update('abc')
print(c['a'], c['b'], c['c'])

d = Counter('aab')
d.update(Counter({'a': 10}))
print(d['a'], d['b'])

e = Counter('aab')
e.update({'b': 4, 'z': 2})
print(e['a'], e['b'], e['z'])

f = Counter('aaab')
f.subtract('ab')
print(f['a'], f['b'])
f.subtract({'a': 2})
print(f['a'])

g = Counter('aab')
print(sorted(g.elements()))

p = {'a': 1}
p.update({'a': 9, 'b': 2})
print(sorted(p.items()))
