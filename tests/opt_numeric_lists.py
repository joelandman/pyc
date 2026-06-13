values = [1.0, 2.0, 3.0, 4.0]
alias = values
for i in range(len(values)):
    alias[i] = alias[i] + 0.5
print("%.1f %.1f %.1f %.1f" % (values[0], values[1], values[2], values[3]))

ints = [0, 0, 0, 0]
for i in range(4):
    ints[i] = i * i
print(ints[0], ints[1], ints[2], ints[3])
