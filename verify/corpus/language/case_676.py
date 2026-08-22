# corpus case — ground truth is CPython at run time (CHARTER I5).
print(len('a\x00b'))
print(['a\x00b'])
print(len(chr(0)))
print(repr(chr(0)))
print(ord(chr(0)))
