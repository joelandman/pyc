# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: __getattr__ fallback for missing vs present attrs.
class Cache:
    def __init__(self):
        self.known = {"a": 1, "b": 2}

    def __getattr__(self, name):
        if name.startswith("get_"):
            return lambda: name
        raise AttributeError(name)


c = Cache()
print(c.known["a"])
print(c.get_anything())
try:
    c.missing
    print("NO-ATTR-ERR")
except AttributeError:
    print("AttributeError")
