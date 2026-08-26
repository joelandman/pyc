def t(label, fn):
    try:
        print(label, '->', repr(fn()))
    except Exception as e:
        print(label, '->', type(e).__name__, e)

t('1//0', lambda: 1 // 0)
t('1%0', lambda: 1 % 0)
t('divmod(1,0)', lambda: divmod(1, 0))
t('1/0', lambda: 1 / 0)
t('0**-1', lambda: 0 ** -1)
t('0.0**-1', lambda: 0.0 ** -1)
t('1.0//0.0', lambda: 1.0 // 0.0)
t('1.0%0.0', lambda: 1.0 % 0.0)
t('pow(0,-1)', lambda: pow(0, -1))
t('pow(2,-1,5)', lambda: pow(2, -1, 5))
t('(2**70)//0', lambda: (2 ** 70) // 0)
t('2**70 % 0', lambda: (2 ** 70) % 0)
t('(-1)**0.5', lambda: (-1) ** 0.5)
t('(-1.0)**0.5', lambda: (-1.0) ** 0.5)
