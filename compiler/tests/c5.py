def mul(k):
    return lambda x: x * k
print(mul(3)(7))
fns = [lambda x, k=k: x + k for k in [1, 2]]
print([f(10) for f in fns])
