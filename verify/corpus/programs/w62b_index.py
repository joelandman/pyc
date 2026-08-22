# W6.2b / I-131 I-132: rindex + list/tuple index start/end.
print("banana".rindex("n", 0, 3))
print("banana".rindex("a", 2, 4))
print("banana".rindex("a"))
print([1, 2, 1].index(1, 1))
print((1, 2, 1).index(1, 1))
print([1, 2, 1].index(1))
print([1, 2, 1].count(1))
try:
    print([1, 2, 1].index(1, 1, 2))
except ValueError as e:
    print(type(e).__name__)
def f(xs):
    return xs.index(1, 1)
print(f([1, 2, 1]))
