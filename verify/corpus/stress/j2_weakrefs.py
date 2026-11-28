# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: weakrefs — WeakRef, WeakValueDictionary, proxy, and callback on
# gc. Print the final state after the target is collected.
import gc
import weakref


def weak_state():
    obj = {"k": 1}
    ref = weakref.ref(obj)
    proxy = weakref.proxy(obj)
    vals = [proxy["k"], ref() is obj]
    del obj
    gc.collect()
    vals.append(ref() is None)
    return vals


print(weak_state())


class Holder:
    pass


d = weakref.WeakValueDictionary()
item = Holder()
d["item"] = item
print("item" in d)
del item
gc.collect()
print("item" in d)


def cb(ref):
    return "collected"


o = Holder()
wr = weakref.ref(o, cb)
del o
gc.collect()
print("done")
