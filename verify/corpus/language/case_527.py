# corpus case — ground truth is CPython at run time (CHARTER I5).
a = [5, 1, 8, 3, 9, 2]
a.sort()
print(a)

b = [5, 1, 8, 3, 9, 2]
b.reverse()
print(b)

c = [5, 1, 8, 3, 9, 2]
c.insert(0, 99)
print(c)

d = [5, 1, 8, 3, 9, 2]
d.remove(8)
print(d)

e = [5, 1, 8, 3, 9, 2]
print(e.index(8))
print(e.count(5))

f = [5, 1, 8, 3, 9, 2]
g = [10, 20]
f.extend(g)
print(f)

h = [5, 1, 8, 3, 9, 2]
h2 = h.copy()
h2.append(100)
print(h)
print(h2)
