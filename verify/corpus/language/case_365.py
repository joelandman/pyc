# corpus case — ground truth is CPython at run time (CHARTER I5).
def f():
    pairs = [['x', 10], ['y', 20]]
    return [k+str(v) for k, v in pairs]
print(f())
