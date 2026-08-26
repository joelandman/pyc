# -*- coding: latin-1 -*-
s = 'éÿÀ'
print(len(s), [ord(c) for c in s])
print(repr(s), ascii(s))
print(repr(s.encode('utf-8')), repr(s.encode('latin-1')))
b = b'\xe9\xff'
print(len(b), list(b))
print(repr('café'.upper()), repr('ß'.upper()))
print(repr(s[::-1]), repr(sorted(s)))
