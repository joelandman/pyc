class Checker:
    def __getattribute__(self, attr):
        if attr in ("__enter__", "__exit__"):
            raise RuntimeError("instance lookup " + attr)
        return object.__getattribute__(self, attr)
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False
with Checker():
    print("ok")
