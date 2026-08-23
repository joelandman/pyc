class Suppress:
    def __enter__(self): return self
    def __exit__(self, t, v, tb):
        print("saw", t.__name__)
        return True
with Suppress():
    raise ValueError("boom")
print("suppressed, continued")
