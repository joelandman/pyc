# corpus case — ground truth is CPython at run time (CHARTER I5).
def make_doubler():
    return lambda x: x * 2
print(make_doubler()(7))

