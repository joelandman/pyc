# corpus case — ground truth is CPython at run time (CHARTER I5).
x=10
def outer():
    x=20
    def bump():
        nonlocal x
        x += 3
    bump()
    print(x)
outer()
print(x)
