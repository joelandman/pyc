def f(a, b, c=3):
    return (a, b, c)
d = {"b": 2}
print(f(1, **d))
print(f(**{"a": 1, "b": 2, "c": 9}))
print({**{"x": 1}, "y": 2})
print({**{"x": 1}, **{"x": 9, "z": 3}})
