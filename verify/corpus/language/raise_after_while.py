def f():
    n = 0
    while n < 1:
        n = n + 1
    raise IndexError
try:
    f()
except IndexError:
    print("ok")
