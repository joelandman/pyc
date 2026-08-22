# corpus case — ground truth is CPython at run time (CHARTER I5).
x=0
def outer():
    x=1
    def middle():
        nonlocal x
        def inner():
            nonlocal x
            x=2
        inner()
        print("middle", x)
    middle()
    print("outer", x)
outer()
print("global", x)
