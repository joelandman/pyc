# corpus case — ground truth is CPython at run time (CHARTER I5).
lst = [1.0, 2.0, 3.0]
del lst[0]
print(lst)
lst2 = [1, 2, 3]
del lst2[-1]
print(lst2)
try:
    del lst2[10]
except IndexError as e:
    print("IndexError:", e)
d = {"a": 1, "b": 2}
del d["a"]
print(d)
try:
    del d["missing"]
except KeyError as e:
    print("KeyError:", e)
lst3 = [1, "a", 2]
del lst3[1]
print(lst3)
