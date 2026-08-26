s = 'ab\x00cd'
print(len(s), repr(s), repr(s.encode('utf-8')), len(s.encode('utf-8')))
print(repr(s[2]), repr(s.split('\x00')), repr(s.find('\x00')))
print(repr(s + '\x00'), len(s + '\x00'))
print(repr(s.upper()), repr(s.replace('\x00', '-')))
b = b'ab\x00cd'
print(len(b), repr(b), repr(b[2]), repr(b.split(b'\x00')))
print(repr(str(b, 'utf-8')))
print(repr('\x00' * 3), len('\x00' * 3))
print(repr('%s|' % s))
print(repr(f'{s}!'))
print(repr(s == 'ab'), repr(s.startswith('ab')))
