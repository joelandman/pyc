samples = ['héllo', '你好世界', 'naïve', 'ß',
           'á', 'á', 'ȩ́']
for s in samples:
    print(repr(s), len(s), [ord(c) for c in s], len(s.encode('utf-8')))
print(repr('héllo'[1]), repr('héllo'[-1]), repr('héllo'[1:3]))
print(repr('你好'[::-1]))
print(repr(max('héllo')), repr(sorted('héllo')))
print(repr('héllo'.upper()), repr('你好'.upper()))
print(repr(list('é')), repr(ord('é')), repr(chr(233)))
