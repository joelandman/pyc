acc = 2**62
for i in range(4):
    acc += acc
    print(repr(acc))
x = 1
x *= 3037000500
x *= 3037000500
print(repr(x))
y = 2**63 - 1
y += 1
print(repr(y), repr(type(y).__name__))
z = 0.0
for i in range(10):
    z += 0.1
print(repr(z))
n = 10
n //= -3
print(repr(n))
m = -7
m %= 3
print(repr(m))
s = ''
for c in ['é', '\U0001F600']:
    s += c * 2
print(repr(s), len(s))
b = b''
b += b'\xff'
print(repr(b))
