# SoA nbody: module-level list_float + default params (same trick as nbody.py
# SYSTEM/PAIRS) so pyc can FListLoad/store. range() index loops, no unpack.
# Same physics/output as tests/nbody.py.

from math import sqrt
import sys

PI = 3.14159265358979323
SOLAR_MASS = 4 * PI * PI
DAYS_PER_YEAR = 365.24

X = [0.0, 4.84143144246472090e+00, 8.34336671824457987e+00,
     1.28943695621391310e+01, 1.53796971148509165e+01]
Y = [0.0, -1.16032004402742839e+00, 4.12479856412430479e+00,
     -1.51111514016986312e+01, -2.59193146099879641e+01]
Z = [0.0, -1.03622044471123109e-01, -4.03523417114321381e-01,
     -2.23307578892655734e-01, 1.79258772950371181e-01]
VX = [0.0,
      1.66007664274403694e-03 * DAYS_PER_YEAR,
      -2.76742510726862411e-03 * DAYS_PER_YEAR,
      2.96460137564761618e-03 * DAYS_PER_YEAR,
      2.68067772490389322e-03 * DAYS_PER_YEAR]
VY = [0.0,
      7.69901118419740425e-03 * DAYS_PER_YEAR,
      4.99852801234917238e-03 * DAYS_PER_YEAR,
      2.37847173959480950e-03 * DAYS_PER_YEAR,
      1.62824170038242295e-03 * DAYS_PER_YEAR]
VZ = [0.0,
      -6.90460016972063023e-05 * DAYS_PER_YEAR,
      2.30417297573763929e-05 * DAYS_PER_YEAR,
      -2.96589568540237556e-05 * DAYS_PER_YEAR,
      -9.51592254519715870e-05 * DAYS_PER_YEAR]
M = [SOLAR_MASS,
     9.54791938424326609e-04 * SOLAR_MASS,
     2.85885980666130812e-04 * SOLAR_MASS,
     4.36624404335156298e-05 * SOLAR_MASS,
     5.15138902046611451e-05 * SOLAR_MASS]

def offset_momentum(x=X, y=Y, z=Z, vx=VX, vy=VY, vz=VZ, m=M):
    px = 0.0
    py = 0.0
    pz = 0.0
    for i in range(5):
        px += vx[i] * m[i]
        py += vy[i] * m[i]
        pz += vz[i] * m[i]
    vx[0] = -px / m[0]
    vy[0] = -py / m[0]
    vz[0] = -pz / m[0]

def energy(x=X, y=Y, z=Z, vx=VX, vy=VY, vz=VZ, m=M):
    e = 0.0
    for i in range(5):
        e += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i])
        for j in range(i + 1, 5):
            dx = x[i] - x[j]
            dy = y[i] - y[j]
            dz = z[i] - z[j]
            e -= (m[i] * m[j]) / sqrt(dx * dx + dy * dy + dz * dz)
    return e

def advance(dt, n, x=X, y=Y, z=Z, vx=VX, vy=VY, vz=VZ, m=M):
    for s in range(n):
        for i in range(5):
            for j in range(i + 1, 5):
                dx = x[i] - x[j]
                dy = y[i] - y[j]
                dz = z[i] - z[j]
                r2 = dx * dx + dy * dy + dz * dz
                mag = dt / (r2 * sqrt(r2))
                mj = m[j] * mag
                vx[i] -= dx * mj
                vy[i] -= dy * mj
                vz[i] -= dz * mj
                mi = m[i] * mag
                vx[j] += dx * mi
                vy[j] += dy * mi
                vz[j] += dz * mi
        for i in range(5):
            x[i] += vx[i] * dt
            y[i] += vy[i] * dt
            z[i] += vz[i] * dt

def main(n):
    offset_momentum()
    print("%.9f" % energy())
    advance(0.01, n)
    print("%.9f" % energy())

if __name__ == '__main__':
    main(int(sys.argv[1]))
