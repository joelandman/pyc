class NoSuppress:
    def __enter__(self): return self
    def __exit__(self, t, v, tb):
        print("cleanup ran")
        return False
try:
    with NoSuppress():
        raise KeyError("k")
except KeyError as e:
    print("propagated:", e)
