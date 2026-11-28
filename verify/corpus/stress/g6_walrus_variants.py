# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: walrus with default-free comprehension, nested loops, walrus in
# while, walrus in a tuple, and walrus in an if with else branches.
data = [[1, 2, 3], [4, 5], [6, 7, 8, 9]]

total = 0
for row in data:
    if (n := len(row)) > 2:
        total += n
print(total)

# walrus inside comprehension + nested loop
grid = [
    [y for y in range(x) if (q := y * 2) % 3 == 0]
    for x in range(1, 5)
]
print(grid)

# walrus in while
i = 0
out = []
while (i := i + 1) < 5:
    out.append(i)
print(out)

# walrus bound to enclosing scope, survives loop
pairs = [(a, b) for a in range(3) for b in range(2) if (s := a + b) < 3]
print(pairs)
