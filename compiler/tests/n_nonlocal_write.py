def outer():
    x=2
    def inner():
        nonlocal x
        x=3
    inner()
    print(x)
outer()
