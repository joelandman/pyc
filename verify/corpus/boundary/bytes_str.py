b = b'\x00\x7f\x80\xff'
print(repr(b), len(b), list(b), repr(b[0]), repr(b[-1]), repr(b[1:3]))
print(repr(bytes([255, 0, 128])))
print(repr(b.hex()), repr(bytes.fromhex('00ff80')))
try:
    print(b + 'x')
except TypeError as e:
    print('TypeError', e)
try:
    print(b'abc' == 'abc')
except Exception as e:
    print(type(e).__name__, e)
print(repr(b'abc'.decode('ascii')), repr('abc'.encode()))
try:
    print(b.decode('ascii'))
except UnicodeDecodeError as e:
    print('UnicodeDecodeError', e.start, e.reason)
print(repr(b.decode('latin-1')))
ba = bytearray(b'abc')
ba[0] = 255
print(repr(ba), repr(bytes(ba)))
print(repr(b'\xff' > b'\x01'), repr(b'' < b'\x00'))
print(repr(memoryview(b)[1]))
