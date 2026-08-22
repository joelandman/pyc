# corpus case — ground truth is CPython at run time (CHARTER I5).
t = ([1.0, 2.0], [0.0, 0.0], 5.0)
([x, y], v, m) = t
v[0] += m * x
print("%.1f" % x)
print("%.1f" % v[0])
print("%.1f" % m)
