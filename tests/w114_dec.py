# W11.4 / I-120: decimal.getcontext / localcontext.
from decimal import Decimal, getcontext, localcontext
print(getcontext().prec)
getcontext().prec = 5
print(Decimal(1) / Decimal(3))
with localcontext() as ctx:
    ctx.prec = 3
    print(Decimal(1) / Decimal(7))
print(getcontext().prec)
getcontext().prec = 28
print(getcontext().prec)
