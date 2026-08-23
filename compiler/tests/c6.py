def counter():
    n = 0
    def bump():
        nonlocal n
        n = n + 1
        return n
    bump(); bump()
    return n, bump()
print(counter())
