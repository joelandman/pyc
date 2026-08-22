# corpus case — ground truth is CPython at run time (CHARTER I5).
s = {1, 2, 3}
s.discard(2)
print(sorted(s))
s.discard(99)
print(sorted(s))
