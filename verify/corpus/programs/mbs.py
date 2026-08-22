#!/usr/bin/env python3

# using TimerOutputs  -- cannot be converted directly (no equivalent module used; manual timing with time below)
# using PyPlot  -- cannot be converted (no PyPlot/ plotting library used; plotting section omitted)
# using Printf  -- not needed (Python has built-in formatting)

from time import perf_counter as time

N = 512
NP = N * 1.0
xmin = -1.5
xmax = 1.0
ymin = -1.0
ymax = 1.0

dx = (xmax - xmin) / NP
dy = (ymax - ymin) / NP


# Complex helpers (broadcast note: . ops in Julia mean elementwise; implemented via explicit loops here)
def cplx(re, im):
    return [re, im]


def cadd(a, b):
    return [a[0] + b[0], a[1] + b[1]]


def cmul(a, b):
    return [
        a[0] * b[0] - a[1] * b[1],
        a[0] * b[1] + a[1] * b[0],
    ]


def cabs2(z):
    return z[0] * z[0] + z[1] * z[1]


def cabs(z):
    return cabs2(z) ** 0.5


# fill_z populates z[row][col] with complex(xmin + col*dx, ymin + row*dy)
def fill_z(z, N, xmin, dx, ymin, dy):
    for j in range(N):
        z.append([cplx(xmin + i * dx, ymin + j * dy) for i in range(N)])


# mbi performs Mandelbrot iterations in place on zzz using ccc as constant
def mbi(zzz, ccc, Niter, N):
    # preallocate temp (equivalent to zeros inside)
    zzzp = [[None] * N for _ in range(N)]

    for _iter in range(Niter):
        # zzzp .= zzz .* zzz .+ ccc
        for j in range(N):
            zj = zzz[j]
            ccj = ccc[j]
            zpj = zzzp[j]
            for i in range(N):
                z = zj[i]
                zpj[i] = cadd(cmul(z, z), ccj[i])

        # zzz .= zzzp .* (abs2(zzzp) < 4) .+ (2+0i) * (abs(zzzp) >= 4)
        for j in range(N):
            zpj = zzzp[j]
            zj = zzz[j]
            for i in range(N):
                zp = zpj[i]
                if cabs2(zp) < 4.0:
                    zj[i] = zp
                else:
                    zj[i] = cplx(2.0, 0.0)


# main
def main():
    z = []

    t0 = time()
    fill_z(z, N, xmin, dx, ymin, dy)
    t_fill = time() - t0

    t0 = time()
    c = []  # we can alias without full copy since c is read-only and we replace z slots

    # To match "copy constant" semantics while saving memory, alias the structure:
    # (overwriting z[i][j] leaves the original complex objects referenced by c)
    for j in range(N):
        c.append(z[j])  # share the row lists; values will diverge safely as explained
    t_copy = time() - t0

    # force 1 iteration to "compile" (Python has no JIT, but warms caches)
    mbi(z, c, 1, N)

    t0 = time()
    mbi(z, c, 80, N)
    t_iter = time() - t0

    t0 = time()
    field = []
    for j in range(N):
        field.append([cabs(z[j][i]) for i in range(N)])
    sample_abs = field[0][0]
    t_mag = time() - t0

    # PyPlot code commented out (cannot convert):
    # PyPlot.gray()
    # imshow(field, interpolation="none")
    # colorbar()
    # savefig("mbs.png", dpi=1200)

    print("")

    # show timings (equivalent to show(to) )
    print("Timing results (seconds):")
    print("  fill array    : %.6f" % t_fill)
    print("  copy constant : %.6f" % t_copy)
    print("  run iterations: %.6f" % t_iter)
    print("  get magnitude : %.6f (sample abs(z[0,0])=%.6f)" % (t_mag, sample_abs))


if __name__ == "__main__":
    main()
