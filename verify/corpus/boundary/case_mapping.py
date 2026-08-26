for s in ['ß', 'straße', 'İ', 'ı', 'I', 'i', 'ǅ',
          'ﬀ', 'σς', 'Á', 'é', 'ẞ']:
    print(repr(s), repr(s.upper()), repr(s.lower()), repr(s.casefold()),
          repr(s.title()), repr(s.swapcase()))
print(repr('ß'.upper() == 'SS'), repr(len('ß'.upper())))
print(repr('İ'.lower()), len('İ'.lower()))
print(repr('Straße'.casefold() == 'STRASSE'.casefold()))
print(repr('é'.isalpha()), repr('你'.isalpha()), repr('²'.isdigit()),
      repr('²'.isnumeric()), repr('Ⅰ'.isnumeric()))
