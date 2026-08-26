import sys
n = 10 ** 5000
try:
    s = str(n)
    print(len(s), s[:5], s[-5:])
except ValueError as e:
    print('ValueError', e)
try:
    print(int('1' * 5000) % 97)
except ValueError as e:
    print('ValueError', e)
print(sys.get_int_max_str_digits())
sys.set_int_max_str_digits(10000)
print(len(str(10 ** 5000)))
sys.set_int_max_str_digits(0)
print(len(str(10 ** 20000)))
print(repr(hex(2**300)), repr(oct(-255)), repr(bin(-5)))
print(len(f'{10**3000:d}'))
print(repr(format(2**70, ',')), repr(format(-2**70, '_x')))
