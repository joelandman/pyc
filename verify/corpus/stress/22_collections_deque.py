# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: deque maxlen rotation, popleft/appendleft, maxlen trimming.
from collections import deque


d = deque([1, 2, 3, 4], maxlen=4)
d.append(5)
print(list(d))
d.appendleft(0)
print(list(d))
d.rotate(-1)
print(list(d))
print(d.maxlen)
d2 = deque(maxlen=2)
d2.append(1)
d2.append(2)
d2.append(3)
print(list(d2))
