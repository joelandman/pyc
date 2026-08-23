def counter():
    n = 0
    def bump():
        return n
    n = 41
    return bump()
print(counter())
