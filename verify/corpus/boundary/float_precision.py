# float literal round-tripping and 17-significant-digit repr
vals = [1e-300, 1e300, 2.2250738585072014e-308, 1.7976931348623157e308,
        0.1, 0.2, 0.1 + 0.2, 1 / 3, 2 / 3, 1.2345678901234567,
        5e-324, 1e16 + 2.0, 0.3333333333333333, 1e22, 1e23]
for v in vals:
    print(repr(v))
print(repr(0.1 + 0.2 == 0.3))
print(repr(float('1.7976931348623157e308')))
print(repr(1e308 * 10))
print(repr(5e-324 / 2))
print(repr(float.fromhex('0x1.fffffffffffffp+1023')))
print((1.0).hex(), (0.1).hex(), (1e300).hex())
