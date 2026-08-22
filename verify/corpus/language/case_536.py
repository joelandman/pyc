# corpus case — ground truth is CPython at run time (CHARTER I5).
d1 = {"a": 1}
d2 = {"b": 2}
merged = {**d1, **d2}
print(merged["a"])
print(merged["b"])
print(len(merged))
merged2 = {**d1, "c": 3}
print(merged2["a"])
print(merged2["c"])
d3 = {"a": 100}
merged3 = {**d1, **d3}
print(merged3["a"])
