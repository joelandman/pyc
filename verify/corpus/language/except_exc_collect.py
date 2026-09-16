import gc
import weakref


class Obj:
    pass


class E(Exception):
    def __init__(self, obj):
        self.obj = obj


def inner(obj):
    local_ref = obj
    raise E(obj)


obj = Obj()
wr = weakref.ref(obj)
try:
    inner(obj)
except E as e:
    pass
obj = None
gc.collect()
print("as", wr() is None)

obj = Obj()
wr = weakref.ref(obj)
try:
    inner(obj)
except:
    pass
obj = None
gc.collect()
print("bare", wr() is None)
