def inner(x):
    return x / 0
def outer(v):
    return inner(v)
print("before")
outer(3)
