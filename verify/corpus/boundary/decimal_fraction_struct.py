from decimal import Decimal, getcontext, InvalidOperation
from fractions import Fraction
import struct
print(repr(Decimal('0.1') + Decimal('0.2')), repr(Decimal(0.1)))
print(repr(Decimal('1') / Decimal('3')), repr(Decimal('Infinity')), repr(Decimal('NaN')))
getcontext().prec = 50
print(repr(Decimal(1) / Decimal(7)))
try:
    print(Decimal(1) / Decimal(0))
except Exception as e:
    print(type(e).__name__, e)
print(repr(Fraction(1, 3) + Fraction(1, 6)), repr(Fraction(0.1)), repr(Fraction(2**70, 3)))
print(repr(Fraction(1, 3).limit_denominator(10)), repr(float(Fraction(1, 3))))
print(repr(struct.pack('<d', 0.1)), repr(struct.unpack('<d', struct.pack('<d', 0.1))))
print(repr(struct.pack('>f', 1e300 if False else 3.4e38)))
print(repr(struct.unpack('<q', b'\xff' * 8)), repr(struct.unpack('<Q', b'\xff' * 8)))
try:
    struct.pack('<b', 200)
except struct.error as e:
    print('struct.error', e)
print(repr(struct.unpack('<f', struct.pack('<f', 0.1))))
