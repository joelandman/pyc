# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: dict/set/set operations and subtleties — set default dedup with
# __hash__/__eq__, dict merge order, frozenset equality, set with mixed
# hashable types.
from fractions import Fraction

# Fraction has custom __hash__ / __eq__
s = {1, 2, Fraction(1, 1)}
print(len(s))  # Fraction(1,1) == 1 -> deduped to one element
print(Fraction(1, 1) in s)

fs = frozenset([1, 2, 3])
print(fs | {3, 4, 5})
print(fs & {2, 3, 9})

d = {"a": 1}
d |= {"b": 2, "c": 3}
print(d["a"], d["b"], d["c"])

# nested dict / set comprehension with shared loop var
pairs = {(i, j) for i in range(2) for j in range(2)}
print(sorted(pairs))
