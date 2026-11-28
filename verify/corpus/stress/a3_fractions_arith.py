# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: Fraction arithmetic auto-reduction and comparisons.
from fractions import Fraction


print(Fraction(4, 6) + Fraction(1, 3))
print(Fraction(2, 3) * Fraction(3, 4))
print(Fraction(1, 2) == Fraction(2, 4))
print(Fraction(7, 3) + 2)
print(Fraction(-1, 4).numer, Fraction(-1, 4).denom)
print(sum(Fraction(i, 1) for i in range(1, 5)))
