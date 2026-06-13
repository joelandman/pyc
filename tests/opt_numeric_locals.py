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
