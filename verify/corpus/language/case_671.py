# corpus case — ground truth is CPython at run time (CHARTER I5).
def fs(s):
    return s.split(maxsplit=1)
print(fs("a b c"))
def ff(s):
    return s.format(x=3)
print(ff("{x}"))
def fsn(s):
    return s.split(None, 1)
print(fsn("a  b  c"))
