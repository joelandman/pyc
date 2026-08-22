# corpus case — ground truth is CPython at run time (CHARTER I5).
from decimal import Decimal

a = Decimal('0.1')
b = Decimal('0.2')
print(a + b)
print(a + b == Decimal('0.3'))

print(Decimal(5))
print(Decimal('3.14159'))
print(Decimal('3.14159') - Decimal('0.14159'))
print(Decimal('2.5') * Decimal('4'))
print(Decimal('1') / Decimal('4'))
print(Decimal('10') // Decimal('3'))
print(-Decimal('3.14'))

print(Decimal('1.5') + 1)
print(1 + Decimal('1.5'))

print(Decimal('1.50') == Decimal('1.5'))
print(Decimal('1.5') < Decimal('1.6'))
print(Decimal('1.5') < 2)
print(Decimal('0') == 0)
print(bool(Decimal('0')))
print(bool(Decimal('0.01')))

d = Decimal('3.14159')
print(d.quantize(Decimal('0.01')))

print(repr(Decimal('3.14')))
print([Decimal('1.5'), Decimal('2.5')])
print(str(Decimal('3.14')))

print(int(Decimal('3.9')))
print(int(Decimal('-3.9')))
print(float(Decimal('3.14')))

print(isinstance(Decimal('1'), Decimal))
print(type(Decimal('1')))

def add_decimals(x, y):
    return x + y
print(add_decimals(Decimal('1.1'), Decimal('2.2')))
