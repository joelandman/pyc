# corpus case — ground truth is CPython at run time (CHARTER I5).
a = [1, 2, 3]
b = [1, 2, 4]
print(a == b)
print(a != b)
c = [1, 2, 3]
print(a == c)
d = [1, 2]
print(a == d)
print(a < d)
print(d < a)
e = [1.0, 2.0, 3.0]
f = [1.0, 2.0, 4.0]
print(e == f)

g = [1, 2, 3]
h = [4, 5]
print(g + h)
print([] + g)
print(g + [])
i = [1, 2]
i += [3, 4]
print(i)
print([1, 2, 3] + [4, "x"])
j = [1, 2]
k = [3.0, 4.0]
print(j + k)
