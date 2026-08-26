try:
    import unicodedata
except ImportError:
    print('no unicodedata')
else:
    pairs = [('é', 'é'), ('ﬀ', 'ff'), ('Å', 'Å')]
    for a, b in pairs:
        for form in ('NFC', 'NFD', 'NFKC', 'NFKD'):
            print(form, ascii(a), ascii(unicodedata.normalize(form, a)),
                  ascii(unicodedata.normalize(form, b)))
        print('eq', a == b, unicodedata.normalize('NFC', a) == unicodedata.normalize('NFC', b))
    print(repr(unicodedata.name('é')), repr(unicodedata.category('é')))
    print(repr(unicodedata.category('\U0001F600')), repr(unicodedata.decimal('7')))
    print(repr(unicodedata.combining('́')), repr(unicodedata.east_asian_width('你')))
