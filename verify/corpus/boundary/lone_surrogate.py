s = '\ud800'
print(len(s), hex(ord(s)))
print(repr(s))
try:
    print(repr(s.encode('utf-8')))
except UnicodeEncodeError as e:
    print('UnicodeEncodeError', e.encoding, e.start, e.end, e.reason)
try:
    print(repr(s.encode('utf-16-le')))
except UnicodeEncodeError as e:
    print('UnicodeEncodeError utf-16', e.reason)
print(repr(s.encode('utf-8', 'replace')))
print(repr(s.encode('utf-8', 'surrogatepass')))
print(repr(b'\xed\xa0\x80'.decode('utf-8', 'surrogatepass')))
try:
    print(repr(b'\xed\xa0\x80'.decode('utf-8')))
except UnicodeDecodeError as e:
    print('UnicodeDecodeError', e.reason)
print(repr('a\udcffb'), len('a\udcffb'))
print(repr('\udfff'), repr('𐀀'), len('𐀀'))
