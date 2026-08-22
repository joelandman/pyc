# corpus case — ground truth is CPython at run time (CHARTER I5).
class DummyCtx:
    def __enter__(self): return 42
    def __exit__(self, *a): pass
with DummyCtx() as x:
    print(x)
