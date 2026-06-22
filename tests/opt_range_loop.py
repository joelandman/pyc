total = 0
for i in range(2, 10, 2):
    total += i
print(total)

descending = 0
for j in range(5, 0, -2):
    descending = descending * 10 + j
print(descending)

skipped = 0
for k in range(6):
    if k == 5:
        break
    skipped += k
print(skipped)

limit = 4
dynamic = 0
for x in range(limit):
    dynamic += x
print(dynamic)
