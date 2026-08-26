def t(label, fn):
    try:
        r = fn()
    except Exception as e:
        print(label, type(e).__name__, str(e))
    else:
        print(label, 'ok', repr(r))

t('int-str', lambda: 1 + 'a')
t('str-int', lambda: 'a' + 1)
t('int(None)', lambda: int(None))
t('float(list)', lambda: float([]))
t('str-idx', lambda: 'abc'[5])
t('bytes-idx', lambda: b'abc'[5])
t('chr(-1)', lambda: chr(-1))
t('ord2', lambda: ord('ab'))
t('ord0', lambda: ord(''))
t('shift-neg', lambda: 1 << -1)
t('bigshift', lambda: 1 << (2**63))
t('int-base', lambda: int('abc'))
t('float-parse', lambda: float('1.0.0'))
t('imag', lambda: int(1j))
