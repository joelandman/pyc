def f(a, b=10, c="z"):
    return (a, b, c)
print(f(1))
print(f(1, 2))
print(f(1, 2, 3))
print(f(1, c=9))
