for s in ['a\tb', 'a\nb', "it's", 'say "hi"', '\\', '\x7f', '\x80', ' ',
          '​', ' ', '\U0001F600', '\x01', 'a\rb', '\'"']:
    print(repr(s), ascii(s), len(s))
print(repr(str(['a\x00b', 'é'])))
print(repr('%r' % 'é'), repr(f'{chr(233)!r}'), repr(f'{chr(233)!a}'))
print(repr(repr(b'a\x00\xff"\'')))
