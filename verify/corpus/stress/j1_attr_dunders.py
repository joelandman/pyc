# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: all five dunder attribute methods together, including
# __getattribute__ (which runs for every access) short-circuiting correctly.
class Weird:
    attrs = {}

    def __init__(self):
        # bypass __getattribute__ by writing into __dict__ via type()
        object.__setattr__(self, "real", 42)

    def __getattribute__(self, name):
        if name in ("real", "__dict__"):
            return super().__getattribute__(name)
        return self.attrs.get(name, "MISSING")

    def __getattr__(self, name):
        # only called if __getattribute__ raises AttributeError
        return f"auto:{name}"

    def __setattr__(self, name, value):
        self.attrs[name] = value

    def __delattr__(self, name):
        self.attrs.pop(name, None)


w = Weird()
print(w.real)
print(w.missing_attr)  # falls to __getattr__
w.new = 1
print(w.new)
del w.new
print(w.new)
print(w.real)
