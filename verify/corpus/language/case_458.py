# corpus case — ground truth is CPython at run time (CHARTER I5).
x=0
def outer():
    x=1
    def middle():
        nonlocal x
        x=2
        def inner():
            nonlocal x
            x=3
        inner()
        print("middle", x)
    middle()
    print("outer", x)
outer()
print("global", x)
