# The Computer Language Benchmarks Game -- spectralnorm.
#
# Perf workload (STATUS.md, real-world performance). Deterministic: no
# input, no clock, no randomness; N is pinned below. Pure float
# arithmetic in Python lists -- the type-stable numeric class where
# unboxing is expected to pay. N is sized for ~1-2 s (the CLBG reference
# N=5500 runs ~3 min in 3.14; time scales as N^2).
N = 500

def mat(i, j):
    return 1.0 / ((i + j) * (i + j + 1) / 2 + i + 1)

def mul(a):
    an = len(a)
    b = [0.0] * an
    for i in range(an):
        s = 0.0
        for j in range(an):
            s += mat(i, j) * a[j]
        b[i] = s
    return b

def t(a):
    an = len(a)
    b = [0.0] * an
    for i in range(an):
        for j in range(i, an):
            b[i], b[j] = a[j], a[i]
    return b

def at(a):
    an = len(a)
    b = [0.0] * an
    for i in range(an):
        s = 0.0
        for j in range(an):
            s += mat(j, i) * a[j]
        b[i] = s
    return b

def inner(a, b):
    s = 0.0
    for i in range(len(a)):
        s += a[i] * b[i]
    return s

def main():
    u = [1.0] * N
    for _ in range(10):
        v = mul(at(mul(t(u))))
        au = mul(t(v))
        av = mul(t(au))
        ub = inner(u, av)
        vb = inner(v, au)
        print(ub / vb)

main()
