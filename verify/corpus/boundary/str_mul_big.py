s = 'é' * 1000
print(len(s), len(s.encode('utf-8')), repr(s[:2]), repr(s[-1]))
t = '\U0001F600' * 500
print(len(t), len(t.encode('utf-8')), len(t.encode('utf-16-le')))
u = (s + t)
print(len(u), repr(u[999]), repr(u[1000]))
print(repr(u.count('é')), repr(u.count('\U0001F600')))
print(len(''.join([s, t, s])))
print(repr(('ab' * 3).join(['x', 'y'])))
print(len(b'\xff' * 1000), repr((b'\xff' * 3).hex()))
print(repr(('é' * 3).encode('utf-8').decode('utf-8') == 'é' * 3))
