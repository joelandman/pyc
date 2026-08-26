for s in ('  42  ', '+7', '-0', '0x1f', '1_000_000', '007', '1e3', 'inf', 'nan', ''):
    for fn in (int, float):
        try:
            print(fn.__name__, repr(s), '->', repr(fn(s)))
        except Exception as e:
            print(fn.__name__, repr(s), '->', type(e).__name__, e)
print(repr(int('0x1f', 16)), repr(int('1f', 16)), repr(int('777', 8)), repr(int('z', 36)))
print(repr(float('  -inf ')), repr(float('Infinity')), repr(float('NaN') != float('NaN')))
try:
    print(int('12', 1))
except ValueError as e:
    print('ValueError', e)
print(repr(bool(0.0)), repr(bool(float('nan'))), repr(int(True) + 1))
print(repr(complex(1, 2)), repr(abs(complex(3, 4))))
