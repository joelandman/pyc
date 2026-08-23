def outer():
    x = 1
    def mid():
        def inner():
            return x
        return inner()
    return mid()
print(outer())
