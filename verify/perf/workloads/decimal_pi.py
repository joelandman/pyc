# pi via the Chudnovsky series at 10000 digits (decimal module).
#
# Perf workload (STATUS.md, real-world performance). Deterministic:
# precision and term count are pinned below. The `decimal` module is the
# reference C-bound numeric workload (its core is C, _decimal) -- the
# class where pyc is expected to trail CPython, which is exactly the
# honest floor the table needs. Chudnovsky: ~14.16 digits per term, so
# TERMS covers PREC with margin.
from decimal import Decimal, getcontext

PREC = 10000
TERMS = 1200

def pi_digits():
    getcontext().prec = PREC + 20
    C = Decimal(426880) * Decimal(10005).sqrt()
    K = Decimal(1)
    M = Decimal(1)
    L = Decimal(13)
    X = Decimal(1)
    S = Decimal(42450)
    for _ in range(TERMS):
        M = (M * (3 * K - 2) * (3 * K - 1)) / (K * K)
        L += 2
        X *= -162560
        S += (M * L * X) / K
        K += 1
    return C / S

print(str(pi_digits())[:PREC + 5])
