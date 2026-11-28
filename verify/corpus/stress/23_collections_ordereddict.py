# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: OrderedDict ordering, move_to_end, __eq__ ignoring order.
from collections import OrderedDict


o = OrderedDict()
o["a"] = 1
o["b"] = 2
o["c"] = 3
o.move_to_end("a")
print(list(o.keys()))
print(list(o.values()))
popped = o.popitem(last=False)
print(popped)
print(list(o.keys()))
print(OrderedDict(b=1, a=2) == OrderedDict(a=2, b=1))
