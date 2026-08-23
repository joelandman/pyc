xs = [2, 3]
print([1, *xs, 4])
print((1, *xs))
print({1, *xs})
def f(a, b, c):
    return a + b + c
print(f(*[1, 2, 3]))
print(f(1, *[2, 3]))
