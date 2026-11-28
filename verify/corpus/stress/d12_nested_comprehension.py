# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: nested comprehension capturing enclosing loop vars (the historical
# cell-leak P0 area). Must read the captured values, not the cell objects.
def grid():
    rows = [[i * j for j in range(3)] for i in range(3)]
    flat = [x for row in rows for x in row]
    captured = [j for j in range(2) for i in range(2) if i + j == 1]
    return rows, flat, captured


def outer(n):
    def inner():
        return [k * n for k in range(n)]
    return inner()


r, flat, cap = grid()
print(r)
print(flat)
print(cap)
print(outer(4))
