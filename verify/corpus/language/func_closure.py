def outer():
    k = 1
    def inner():
        return k
    return inner
c = outer().__closure__
print(c is not None, len(c), type(c[0]).__name__)
