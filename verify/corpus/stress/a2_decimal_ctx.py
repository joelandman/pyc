# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: Decimal arithmetic, context precision, division rounding.
from decimal import Decimal, getcontext


getcontext().prec = 6
print(Decimal(1) / Decimal(3))
print(Decimal("2.5") + Decimal("1.25"))
print(Decimal("1.00") * Decimal("1.05"))
print(Decimal("8") ** Decimal("3"))
print(getcontext().prec)
