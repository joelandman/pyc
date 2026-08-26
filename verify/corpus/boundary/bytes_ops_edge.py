b = b'\x00\xff\x80abc'
print(repr(b.upper()), repr(b.split()), repr(b.strip(b'\x00')))
print(repr(b * 2), repr(b[::-1]), repr(b.replace(b'\xff', b'\x00\x01')))
print(repr(b.find(b'\x80')), repr(b.count(b'\x00')), repr(b.startswith(b'\x00')))
print(repr(b'%s|%d' % (b'\xff', 42)), repr(b'{}'.__class__))
print(repr(bytes(3)), repr(bytes(range(250, 256))))
print(repr(b.translate(bytes.maketrans(b'\x00', b'Z'))))
print(repr(int.from_bytes(b'\xff\x00', 'big')), repr(int.from_bytes(b'\xff\x00', 'little', signed=True)))
print(repr((-1).to_bytes(2, 'big', signed=True)), repr((2**70).to_bytes(16, 'little')))
try:
    print((300).to_bytes(1, 'big'))
except OverflowError as e:
    print('OverflowError', e)
print(repr(b'abc'.hex('-')), repr(bytearray(b'\xff')[0]))
print(repr(ascii(b'\x00\xff')), repr(str(b'\xff')))
