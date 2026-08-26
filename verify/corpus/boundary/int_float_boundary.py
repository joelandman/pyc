print(repr(2**53), repr(float(2**53)), repr(float(2**53 + 1)))
print(repr(int(float(2**53 + 1))))
print(repr(float(2**53) + 1.0 == float(2**53)))
print(repr(2**53 + 1 == float(2**53)))
print(repr(int(3.99)), repr(int(-3.99)), repr(int(-0.5)))
for x in (0.5, 1.5, 2.5, 3.5, -0.5, -1.5, -2.5, 0.15, 2.675):
    print(x, repr(round(x)), repr(round(x, 1)))
print(repr(round(2.5)), repr(round(1234.5678, -2)))
try:
    print(int(float('inf')))
except OverflowError as e:
    print('OverflowError', e)
try:
    print(int(float('nan')))
except ValueError as e:
    print('ValueError', e)
print(repr(float(10**308)))
try:
    print(float(10**400))
except OverflowError as e:
    print('OverflowError', e)
