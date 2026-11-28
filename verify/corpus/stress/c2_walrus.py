# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: walrus operator inside comprehensions and nested loops.
def run(data):
    total = 0
    for chunk in data:
        if (n := len(chunk)) > 2:
            total += n
    evens = [x for x in range(10) if (r := x % 2) == 0]
    grouped = {k: [y for y in range(k) if (z := y * 2) % 3 == 0] for k in (2, 3)}
    return total, evens, grouped


print(run([[1, 2], [3, 4, 5], [6]]))
print(run([[1], [2, 3]]))
