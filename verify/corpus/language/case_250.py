# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(): pass
print(callable(f), callable(42), callable('x'), callable([]))
