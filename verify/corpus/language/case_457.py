# corpus case — ground truth is CPython at run time (CHARTER I5).
x=1
def outer():
    x=2
    def inner():
        nonlocal x
        x=3
    inner()
    print(x)
outer()
print(x)
