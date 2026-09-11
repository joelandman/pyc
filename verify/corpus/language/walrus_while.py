def f():
    a = 9
    n = 2
    x = 3
    while a > (d := x // a**(n-1)):
        a = ((n-1)*a + d) // n
    return a, d
print(f())
