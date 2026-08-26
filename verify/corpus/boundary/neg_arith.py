pairs = [(7, 3), (-7, 3), (7, -3), (-7, -3), (10, 5), (-10, 5), (0, 5), (-1, 7)]
for a, b in pairs:
    print(a, b, repr(a // b), repr(a % b), repr(divmod(a, b)))
for a, b in [(7.0, 3.0), (-7.0, 3.0), (7.0, -3.0), (-7.5, 2.5)]:
    print(a, b, repr(a // b), repr(a % b), repr(divmod(a, b)))
print(repr(2 ** -1), repr((-2) ** -1), repr((-8) ** (1/3)))
print(repr(-3 ** 2), repr((-3) ** 2), repr(2 ** 3 ** 2))
for v in (-1, -5, -256, 5):
    print(v, repr(v & 0xFF), repr(v | 0xF0), repr(v ^ -1), repr(~v), repr(v >> 3), repr(v << 3))
print(repr(-1 >> 100), repr(-(2**70) >> 65))
