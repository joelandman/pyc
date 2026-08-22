def pair(x, y):
    return [x, y]

def use_pair(flag):
    xs = [pair(1.5, 2.5) for _ in range(2)]
    if flag:
        xs[0] = pair(3.0, 4.0)
    return xs[0]

print(use_pair(True))
