def local_sum():
    x = 1000
    y = 2000
    return x + y

def escaped():
    xs = []
    a = 1000
    b = 2001
    xs.append(a)
    xs.append(b)
    return xs

def inner(n):
    return n + 1000

def outer():
    return inner(5) + inner(6)

print(local_sum())
print(escaped())
print(escaped()[0] + escaped()[1])
print(outer())
