# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: property getter/setter/deleter interplay.
class Temp:
    def __init__(self):
        self._c = 0.0

    @property
    def celsius(self):
        return self._c

    @celsius.setter
    def celsius(self, value):
        self._c = value

    @celsius.deleter
    def celsius(self):
        self._c = -273.15


t = Temp()
t.celsius = 100
print(t.celsius)
t.celsius = 0
print(t.celsius)
del t.celsius
print(t.celsius)
