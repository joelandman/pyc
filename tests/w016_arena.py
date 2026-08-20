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

def local_list():
    xs = [1000, 2001]
    return xs[0] + xs[1]

def escape_list():
    xs = [1000, 2002]
    return xs

def escape_mixed():
    xs = [1000, "z"]
    return xs

def local_mixed():
    xs = [1000, "z"]
    return xs[0]

def churn():
    x = 9999
    y = 8888
    return x + y

print(local_sum())
print(escaped())
print(escaped()[0] + escaped()[1])
print(outer())
print(local_list())
print(escape_list())
r = escape_mixed()
print(churn())
print(r)
print(local_mixed())
