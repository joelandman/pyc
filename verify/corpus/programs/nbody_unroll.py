# Fully unrolled 5-body nbody: scalar floats only in the step kernel.
# Same physics/output as tests/nbody.py.

from math import sqrt
import sys

PI = 3.14159265358979323
SOLAR_MASS = 4 * PI * PI
DAYS_PER_YEAR = 365.24

def nbody(n):
    x0 = 0.0
    y0 = 0.0
    z0 = 0.0
    vx0 = 0.0
    vy0 = 0.0
    vz0 = 0.0
    m0 = SOLAR_MASS

    x1 = 4.84143144246472090e+00
    y1 = -1.16032004402742839e+00
    z1 = -1.03622044471123109e-01
    vx1 = 1.66007664274403694e-03 * DAYS_PER_YEAR
    vy1 = 7.69901118419740425e-03 * DAYS_PER_YEAR
    vz1 = -6.90460016972063023e-05 * DAYS_PER_YEAR
    m1 = 9.54791938424326609e-04 * SOLAR_MASS

    x2 = 8.34336671824457987e+00
    y2 = 4.12479856412430479e+00
    z2 = -4.03523417114321381e-01
    vx2 = -2.76742510726862411e-03 * DAYS_PER_YEAR
    vy2 = 4.99852801234917238e-03 * DAYS_PER_YEAR
    vz2 = 2.30417297573763929e-05 * DAYS_PER_YEAR
    m2 = 2.85885980666130812e-04 * SOLAR_MASS

    x3 = 1.28943695621391310e+01
    y3 = -1.51111514016986312e+01
    z3 = -2.23307578892655734e-01
    vx3 = 2.96460137564761618e-03 * DAYS_PER_YEAR
    vy3 = 2.37847173959480950e-03 * DAYS_PER_YEAR
    vz3 = -2.96589568540237556e-05 * DAYS_PER_YEAR
    m3 = 4.36624404335156298e-05 * SOLAR_MASS

    x4 = 1.53796971148509165e+01
    y4 = -2.59193146099879641e+01
    z4 = 1.79258772950371181e-01
    vx4 = 2.68067772490389322e-03 * DAYS_PER_YEAR
    vy4 = 1.62824170038242295e-03 * DAYS_PER_YEAR
    vz4 = -9.51592254519715870e-05 * DAYS_PER_YEAR
    m4 = 5.15138902046611451e-05 * SOLAR_MASS

    px = vx0 * m0 + vx1 * m1 + vx2 * m2 + vx3 * m3 + vx4 * m4
    py = vy0 * m0 + vy1 * m1 + vy2 * m2 + vy3 * m3 + vy4 * m4
    pz = vz0 * m0 + vz1 * m1 + vz2 * m2 + vz3 * m3 + vz4 * m4
    vx0 = -px / m0
    vy0 = -py / m0
    vz0 = -pz / m0

    e = 0.5 * (m0 * (vx0 * vx0 + vy0 * vy0 + vz0 * vz0)
             + m1 * (vx1 * vx1 + vy1 * vy1 + vz1 * vz1)
             + m2 * (vx2 * vx2 + vy2 * vy2 + vz2 * vz2)
             + m3 * (vx3 * vx3 + vy3 * vy3 + vz3 * vz3)
             + m4 * (vx4 * vx4 + vy4 * vy4 + vz4 * vz4))
    dx = x0 - x1; dy = y0 - y1; dz = z0 - z1
    e -= (m0 * m1) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x0 - x2; dy = y0 - y2; dz = z0 - z2
    e -= (m0 * m2) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x0 - x3; dy = y0 - y3; dz = z0 - z3
    e -= (m0 * m3) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x0 - x4; dy = y0 - y4; dz = z0 - z4
    e -= (m0 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x1 - x2; dy = y1 - y2; dz = z1 - z2
    e -= (m1 * m2) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x1 - x3; dy = y1 - y3; dz = z1 - z3
    e -= (m1 * m3) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x1 - x4; dy = y1 - y4; dz = z1 - z4
    e -= (m1 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x2 - x3; dy = y2 - y3; dz = z2 - z3
    e -= (m2 * m3) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x2 - x4; dy = y2 - y4; dz = z2 - z4
    e -= (m2 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x3 - x4; dy = y3 - y4; dz = z3 - z4
    e -= (m3 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    print("%.9f" % e)

    dt = 0.01
    for s in range(n):
        dx = x0 - x1; dy = y0 - y1; dz = z0 - z1
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m1 * mag; mi = m0 * mag
        vx0 -= dx * mj; vy0 -= dy * mj; vz0 -= dz * mj
        vx1 += dx * mi; vy1 += dy * mi; vz1 += dz * mi

        dx = x0 - x2; dy = y0 - y2; dz = z0 - z2
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m2 * mag; mi = m0 * mag
        vx0 -= dx * mj; vy0 -= dy * mj; vz0 -= dz * mj
        vx2 += dx * mi; vy2 += dy * mi; vz2 += dz * mi

        dx = x0 - x3; dy = y0 - y3; dz = z0 - z3
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m3 * mag; mi = m0 * mag
        vx0 -= dx * mj; vy0 -= dy * mj; vz0 -= dz * mj
        vx3 += dx * mi; vy3 += dy * mi; vz3 += dz * mi

        dx = x0 - x4; dy = y0 - y4; dz = z0 - z4
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m4 * mag; mi = m0 * mag
        vx0 -= dx * mj; vy0 -= dy * mj; vz0 -= dz * mj
        vx4 += dx * mi; vy4 += dy * mi; vz4 += dz * mi

        dx = x1 - x2; dy = y1 - y2; dz = z1 - z2
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m2 * mag; mi = m1 * mag
        vx1 -= dx * mj; vy1 -= dy * mj; vz1 -= dz * mj
        vx2 += dx * mi; vy2 += dy * mi; vz2 += dz * mi

        dx = x1 - x3; dy = y1 - y3; dz = z1 - z3
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m3 * mag; mi = m1 * mag
        vx1 -= dx * mj; vy1 -= dy * mj; vz1 -= dz * mj
        vx3 += dx * mi; vy3 += dy * mi; vz3 += dz * mi

        dx = x1 - x4; dy = y1 - y4; dz = z1 - z4
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m4 * mag; mi = m1 * mag
        vx1 -= dx * mj; vy1 -= dy * mj; vz1 -= dz * mj
        vx4 += dx * mi; vy4 += dy * mi; vz4 += dz * mi

        dx = x2 - x3; dy = y2 - y3; dz = z2 - z3
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m3 * mag; mi = m2 * mag
        vx2 -= dx * mj; vy2 -= dy * mj; vz2 -= dz * mj
        vx3 += dx * mi; vy3 += dy * mi; vz3 += dz * mi

        dx = x2 - x4; dy = y2 - y4; dz = z2 - z4
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m4 * mag; mi = m2 * mag
        vx2 -= dx * mj; vy2 -= dy * mj; vz2 -= dz * mj
        vx4 += dx * mi; vy4 += dy * mi; vz4 += dz * mi

        dx = x3 - x4; dy = y3 - y4; dz = z3 - z4
        r2 = dx * dx + dy * dy + dz * dz
        mag = dt * (r2 ** -1.5)
        mj = m4 * mag; mi = m3 * mag
        vx3 -= dx * mj; vy3 -= dy * mj; vz3 -= dz * mj
        vx4 += dx * mi; vy4 += dy * mi; vz4 += dz * mi

        x0 += vx0 * dt; y0 += vy0 * dt; z0 += vz0 * dt
        x1 += vx1 * dt; y1 += vy1 * dt; z1 += vz1 * dt
        x2 += vx2 * dt; y2 += vy2 * dt; z2 += vz2 * dt
        x3 += vx3 * dt; y3 += vy3 * dt; z3 += vz3 * dt
        x4 += vx4 * dt; y4 += vy4 * dt; z4 += vz4 * dt

    e = 0.5 * (m0 * (vx0 * vx0 + vy0 * vy0 + vz0 * vz0)
             + m1 * (vx1 * vx1 + vy1 * vy1 + vz1 * vz1)
             + m2 * (vx2 * vx2 + vy2 * vy2 + vz2 * vz2)
             + m3 * (vx3 * vx3 + vy3 * vy3 + vz3 * vz3)
             + m4 * (vx4 * vx4 + vy4 * vy4 + vz4 * vz4))
    dx = x0 - x1; dy = y0 - y1; dz = z0 - z1
    e -= (m0 * m1) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x0 - x2; dy = y0 - y2; dz = z0 - z2
    e -= (m0 * m2) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x0 - x3; dy = y0 - y3; dz = z0 - z3
    e -= (m0 * m3) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x0 - x4; dy = y0 - y4; dz = z0 - z4
    e -= (m0 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x1 - x2; dy = y1 - y2; dz = z1 - z2
    e -= (m1 * m2) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x1 - x3; dy = y1 - y3; dz = z1 - z3
    e -= (m1 * m3) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x1 - x4; dy = y1 - y4; dz = z1 - z4
    e -= (m1 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x2 - x3; dy = y2 - y3; dz = z2 - z3
    e -= (m2 * m3) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x2 - x4; dy = y2 - y4; dz = z2 - z4
    e -= (m2 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    dx = x3 - x4; dy = y3 - y4; dz = z3 - z4
    e -= (m3 * m4) / sqrt(dx * dx + dy * dy + dz * dz)
    print("%.9f" % e)

def main(n):
    nbody(n)

if __name__ == '__main__':
    main(int(sys.argv[1]))
