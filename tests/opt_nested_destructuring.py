bodies = [
    ([1.0, 2.0, 3.0], [0.0, 0.0, 0.0], 2.0),
    ([4.0, 6.0, 8.0], [1.0, 1.0, 1.0], 3.0),
]
pairs = [(bodies[0], bodies[1])]

for (([x1, y1, z1], v1, m1), ([x2, y2, z2], v2, m2)) in pairs:
    dx = x1 - x2
    dy = y1 - y2
    dz = z1 - z2
    v1[0] -= dx * m2
    v1[1] -= dy * m2
    v1[2] -= dz * m2
    v2[0] += dx * m1
    v2[1] += dy * m1
    v2[2] += dz * m1

print("%.1f %.1f %.1f" % (bodies[0][1][0], bodies[0][1][1], bodies[0][1][2]))
print("%.1f %.1f %.1f" % (bodies[1][1][0], bodies[1][1][1], bodies[1][1][2]))
