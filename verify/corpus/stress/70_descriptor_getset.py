# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: data descriptor __get__/__set__ overriding instance dict.
class Desc:
    def __init__(self, name):
        self.name = name
        self.stored = 0

    def __get__(self, obj, objtype=None):
        return self.stored

    def __set__(self, obj, value):
        self.stored = value * 2


class Holder:
    d = Desc("d")


h = Holder()
h.d = 5
print(h.d)
h.d = 10
print(h.d)
