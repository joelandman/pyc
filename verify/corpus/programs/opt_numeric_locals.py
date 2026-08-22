acc = 0.0
scale = 1.5
for i in range(8):
    x = float(i) * scale
    acc = acc + x / 2.0
print("%.3f" % acc)

n = 1
for i in range(1, 6):
    n = n * i
print(n)

m = 10
for i in range(4):
    m = m - i
print(m)

a = abs(-7)
print(a * 3)

selector = False
mixed = 1
if selector:
    mixed = "name"
else:
    mixed = 5
print(mixed + 2)
