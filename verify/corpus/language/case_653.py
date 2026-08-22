# corpus case — ground truth is CPython at run time (CHARTER I5).
def fs(s):
    return s.split(None)
def fr(s):
    return s.rsplit(None)
print(fs("a  b"))
print(fr("a  b  c"))
print(fs("  a b  "))
