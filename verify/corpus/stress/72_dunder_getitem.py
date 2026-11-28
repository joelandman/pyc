# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: __getitem__/__setitem__/__delitem__ and __missing__.
class Map:
    def __init__(self):
        self._data = {}

    def __getitem__(self, key):
        return self._data[key]

    def __setitem__(self, key, value):
        self._data[key] = value * 2

    def __delitem__(self, key):
        del self._data[key]


m = Map()
m["x"] = 5
print(m["x"])
m["y"] = 10
print(m["y"])
del m["x"]
try:
    m["x"]
    print("STILL-THERE")
except KeyError:
    print("KeyError")
