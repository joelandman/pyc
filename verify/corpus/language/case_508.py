# corpus case — ground truth is CPython at run time (CHARTER I5).
from collections import Counter
c = Counter('abracadabra')
print(c.most_common(2))
print(c.most_common())
