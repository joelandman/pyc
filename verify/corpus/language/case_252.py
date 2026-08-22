# corpus case — ground truth is CPython at run time (CHARTER I5).
class Bar:
    def __call__(self): return 1
print(callable(Bar()))
