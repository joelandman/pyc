import functools
@staticmethod
def s(): return 1
class C:
    @property
    def v(self): return 42
print(C().v)
