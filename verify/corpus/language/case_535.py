# corpus case — ground truth is CPython at run time (CHARTER I5).
f = [1, 2, 3]
g = tuple(f)
print(g[0], g[1], g[2])
print(len(g))
print(g)
print(tuple("abc"))
q, r = divmod(17, 5)
print(q, r)
print(divmod(17, 5))
print(pow(2, 10))
print(pow(2, 10, 1000))
print(pow(3, 5, 7))
print(pow(2, 10, -7))
try:
    pow(2, 10, 0)
except ValueError as e:
    print("ValueError:", e)
