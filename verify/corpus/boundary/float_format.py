vals = [0.1, 1/3, 1e300, 1e-300, 2.675, 1234567.891, -0.0, float('inf'),
        float('-inf'), float('nan'), 1e16, 1e17, 123456789012345678.0, 0.5, 2.5]
for v in vals:
    print(repr(v), str(v), '%g' % v, '%f' % v, '%e' % v, '%.17g' % v, '%s' % v)
    print(format(v, '.3f'), format(v, '.0f'), format(v, 'g'), format(v, 'e'),
          format(v, '.20f'), format(v, '#.0f'), format(v, '+.2f'))
    print(f'{v:12.4f}|{v:<12g}|{v:^12}|{v:_>10.2e}')
print('%.0f %.0f %.0f %.0f' % (0.5, 1.5, 2.5, 3.5))
print(format(1e16, '.0f'), format(1e22, 'f'))
