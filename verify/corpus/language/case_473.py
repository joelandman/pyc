# corpus case — ground truth is CPython at run time (CHARTER I5).
def add(a, b):
    return a + b
print(str(add)[:14])
g = add
print(g is add, g == add)
def sub(a, b):
    return a - b
print(add == sub)
sq = lambda x: x * x
print(str(sq)[:19])
print(sq(7))
if add:
    print("truthy")
