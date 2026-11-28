# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: ChainMap layered lookups, new_child, update maps.
from collections import ChainMap


base = {"a": 1, "b": 2}
local = {"b": 20, "c": 30}
cm = ChainMap(local, base)
print(cm["a"])
print(cm["b"])
print(cm["c"])
child = cm.new_child({"d": 40})
print(child["d"])
print(cm["d"])
cm.update({"a": 100})
print(cm["a"])
