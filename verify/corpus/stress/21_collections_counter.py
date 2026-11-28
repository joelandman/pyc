# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: Counter counting, most_common, arithmetic, subtract.
from collections import Counter


c = Counter("abbbccd")
print(c["a"])
print(c["z"])
print([k for k, _ in c.most_common()])
c2 = Counter(a=1, b=2)
c += c2
print(c["a"], c["b"])
c.subtract(["a", "a"])
print(c["a"], c["b"])
print(sorted(c.elements()))
