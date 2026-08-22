# corpus case — ground truth is CPython at run time (CHARTER I5).
class Container:
    def __init__(self):
        self.items = {}
    def __getitem__(self, key):
        return self.items.get(key, 'missing')
    def __setitem__(self, key, value):
        self.items[key] = value
    def __contains__(self, key):
        return key in self.items
c = Container()
c['a'] = 1
print(c['a'], c['b'], 'a' in c, 'b' in c)
