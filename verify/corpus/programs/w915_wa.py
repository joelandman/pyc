# W9.15 remaining wrong-answers: I-161 I-164 I-165 I-166 I-174 I-175 I-176 I-194–I-198.
class C:
    pass
class E:
    def __getitem__(self, k):
        return k
    def __setitem__(self, k, v):
        self.last = (k, v)
class F:
    def __delitem__(self, k):
        self.gone = k

# I-194
import sys
try:
    print(sum(sys))
except TypeError as e:
    print(e)
print(sum([1, 2, 3]))

# I-195
try:
    print(sys["argv"])
except TypeError as e:
    print(type(e).__name__)
print({1: 2}[1])
print(sys.argv is not None)

# I-196
try:
    C["x"] = 1
    print("assigned")
except TypeError as e:
    print(type(e).__name__)
try:
    del C["__mro__"]
    print("deleted")
except TypeError as e:
    print(type(e).__name__)
try:
    sys["argv"] = 1
    print("sysassigned")
except TypeError as e:
    print(type(e).__name__)
d = {1: 2}
d[3] = 4
print(d[3])
e = E()
e["k"] = 9
print(e.last)

# I-197
try:
    print(sorted(C()))
except TypeError as e:
    print(type(e).__name__)
try:
    print(sorted(sys))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(map(str, C())))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(filter(None, C())))
except TypeError as e:
    print(type(e).__name__)
try:
    print(set(C()))
except TypeError as e:
    print(type(e).__name__)
print(sorted({3: 0, 1: 0}))
try:
    print(list(C))
except TypeError as e:
    print(type(e).__name__)

# I-198
try:
    print(E["x"])
except TypeError as e:
    print(type(e).__name__)
print(E()["x"])

# I-161
print(list(zip({1: 2}, [9])))
print(list(zip({1}, [9])))
try:
    print(list(zip(True, [1])))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(zip(1.0, [1])))
except TypeError as e:
    print(type(e).__name__)
print(list(zip([1, 2], [3, 4])))

# I-164
print(list(enumerate({1: 2})))
print(list(enumerate({7})))
try:
    print(list(enumerate(True)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(enumerate(1.0)))
except TypeError as e:
    print(type(e).__name__)
print(list(enumerate((1, 2))))

# I-165
en = enumerate
print(list(en([1, 2], 5)))
try:
    print(en(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(en())
except TypeError as e:
    print(type(e).__name__)
print(list(enumerate([1, 2], 5)))

# I-166
try:
    print(list(enumerate([1], start="x")))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(enumerate([1], start=1.5)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(enumerate([1], start=None)))
except TypeError as e:
    print(type(e).__name__)
print(list(enumerate([1], start=True)))

# I-174
try:
    print(any(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(any(1))
except TypeError as e:
    print(type(e).__name__)
try:
    print(all(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(sorted(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(sum(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(min(None))
except TypeError as e:
    print(type(e).__name__)
print(any({1: 0}))
print(min({3, 1, 2}))
print(any([0, 1]))

# I-175
from functools import cmp_to_key
def cmp(a, b):
    return (a > b) - (a < b)
print(sorted([3, 1, 2], key=cmp_to_key(cmp)))
k = cmp_to_key(cmp)
print(sorted([3, 1, 2], key=k))
from functools import cmp_to_key as ctk
print(sorted([3, 1, 2], key=ctk(cmp)))
import functools
print(sorted([3, 1, 2], key=functools.cmp_to_key(cmp)))
print(sorted([3, 1, 2]))

# I-176
try:
    print(sum([], ""))
except TypeError as e:
    print(e)
try:
    print(sum([], b""))
except TypeError as e:
    print(e)
try:
    print(sum([], bytearray()))
except TypeError as e:
    print(e)
print(sum([], 10))
print(sum([1, 2], 10))
