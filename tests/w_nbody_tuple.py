def combinations(l):
    result = []
    for x in range(len(l) - 1):
        ls = l[x+1:]
        for y in ls:
            result.append((l[x], y))
    return result

BODIES = {
    'a': ([1.0, 0.0, 0.0], [0.0, 0.0, 0.0], 2.0),
    'b': ([0.0, 1.0, 0.0], [0.0, 0.0, 0.0], 3.0),
}
SYSTEM = list(BODIES.values())
PAIRS = combinations(SYSTEM)

def advance(dt, n, bodies=SYSTEM, pairs=PAIRS):
    for i in range(n):
        for (([x1, y1, z1], v1, m1),
             ([x2, y2, z2], v2, m2)) in pairs:
            v1[0] += dt * (x2 - x1) * m2
            v2[0] += dt * (x1 - x2) * m1
    print("%.6f" % bodies[0][1][0])
    print("%.6f" % bodies[1][1][0])

advance(0.5, 1)
