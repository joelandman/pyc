s = 'Aé你\U0001F600'
for enc in ('utf-8', 'utf-16', 'utf-32', 'utf-16-le', 'utf-16-be', 'utf-8-sig'):
    b = s.encode(enc)
    print(enc, repr(b), repr(b.decode(enc) == s))
try:
    print(repr(s.encode('latin-1')))
except UnicodeEncodeError as e:
    print('UnicodeEncodeError', e)
print(repr('Aé'.encode('latin-1')))
print(repr(b'\xff\xfe'.decode('latin-1')))
for errs in ('replace', 'ignore', 'backslashreplace', 'xmlcharrefreplace'):
    print(errs, repr(s.encode('latin-1', errs)))
try:
    b'\xff\xfe\xfa'.decode('utf-8')
except UnicodeDecodeError as e:
    print('UnicodeDecodeError', e.encoding, e.start, e.end, e.reason)
for errs in ('replace', 'ignore'):
    print(errs, repr(b'a\xffb'.decode('utf-8', errs)))
print(repr(b'a\xffb'.decode('utf-8', 'surrogateescape')))
print(repr(b'a\xffb'.decode('utf-8', 'surrogateescape').encode('utf-8', 'surrogateescape')))
