# corpus case — ground truth is CPython at run time (CHARTER I5).
from collections import deque, namedtuple, defaultdict

d = deque([1, 2, 3, 4, 5])
print(list(d))
d.appendleft(0)
print(list(d))
print(d.popleft())
print(list(d))
d.rotate(1)
print(list(d))
d.rotate(-2)
print(list(d))
d.pop()
print(list(d))

Point = namedtuple('Point', ['x', 'y'])
p = Point(3, 4)
print(p.x, p.y)
p2 = Point(10, 20)
print(p2.x + p2.y)

dd = defaultdict(list)
dd['a'].append(1)
dd['a'].append(2)
dd['b'].append(3)
print(dd['a'])
print(dd['b'])
print(dd['c'])

dd2 = defaultdict(int)
dd2['x'] += 5
dd2['x'] += 2
print(dd2['x'])
print(dd2['y'])
