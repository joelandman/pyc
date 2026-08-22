# corpus case — ground truth is CPython at run time (CHARTER I5).
def add(a, b):
    return a + b
def loop(start):
    s = start
    for i in range(5):
        s = add(s, i)
    return s
print(loop(0))
print(add(True, 1))
print(add(1, True))
g = add
print(g(3, 4))
print(add(1.5, 2))
print(add(1, 2.5))
s = 0
def bump():
    global s
    s = add(s, 1)
bump()
bump()
print(s)
