def f(x, y):
    return x * y
def g(x):
    return x + 1
a = 9223372036854775807
print(repr(f(a, 2)), repr(g(a)))
print(repr(f(3037000500, 3037000500)))
acc = 1
for i in range(1, 26):
    acc = acc * i
print(repr(acc))
tot = 0
for i in range(64):
    tot = tot * 2 + 1
print(repr(tot))
n = 1
while n < 2**80:
    n <<= 7
print(repr(n))
print(repr(sum(2**62 for _ in range(8))))
try:
    print(repr(len(range(0, 2**70, 3))))
except OverflowError as e:
    print('OverflowError', e)
print(repr(range(0, 2**70, 3)[10**6]))
print(repr(list(range(2**64, 2**64 + 3))))
