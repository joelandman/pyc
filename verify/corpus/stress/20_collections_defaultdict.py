# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: defaultdict factory on missing key, nested factories, missing().
from collections import defaultdict


dd = defaultdict(list)
dd["a"].append(1)
dd["a"].append(2)
print(sorted(dd["a"]))
print(sorted(dd["missing"]))
print(dd.default_factory)

nd = defaultdict(lambda: defaultdict(int))
nd["x"]["y"] += 1
print(nd["x"]["y"])
print(sorted(nd["y"].keys()))


class D(dict):
    def __missing__(self, key):
        return key * 2


d = D({1: "one"})
print(d[1])
print(d[2])
