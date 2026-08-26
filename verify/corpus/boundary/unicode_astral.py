s = 'a\U0001F600b\U0001F1FA\U0001F1F8c'
print(repr(s), len(s))
print([hex(ord(c)) for c in s])
print(repr(s[1]), repr(s[2]), repr(s[1:3]), repr(s[::-1]))
print(len(s.encode('utf-8')), len(s.encode('utf-16-le')), len(s.encode('utf-32-le')))
print(repr(chr(0x10FFFF)), hex(ord(chr(0x10FFFF))))
try:
    print(chr(0x110000))
except ValueError as e:
    print('ValueError', e)
print(repr('\U0001F600'.encode('utf-8')))
print(repr(b'\xf0\x9f\x98\x80'.decode('utf-8')))
print(repr('\U0001F600' * 2), len('\U0001F600' * 2))
print(repr(chr(0xFFFF)), repr(chr(0x10000)))
