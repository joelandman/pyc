print(repr('\N{GREEK SMALL LETTER ALPHA}'), repr('\N{SNOWMAN}'))
print(repr('\x41\101\0\7\77\377'), len('\377'))
print(repr('é\U0001F600'), repr('\\u00e9'))
print(repr(r'\x41\n'), repr(rb'\x41'), repr(b'\x41\101\0\377'))
print(repr('a' 'b' '\x63'), repr(b'a' b'\xff'))
print(repr('''tri
ple'''), repr("line\
cont"))
print(repr('\a\b\f\v\t\r\n'))
print(repr('\N{LATIN SMALL LETTER E WITH ACUTE}' == 'é'))
print(repr(u'x'), repr(R'\d'), repr(BR'\d'), repr(f'{1}'))
print(repr('\x00' '\x00'), len('\x00\x00'))
print(repr('☃'), repr(len('日本語')))
