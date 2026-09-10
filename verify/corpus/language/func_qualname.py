
def outer():
    def inner():
        global g
        def g():
            def h():
                pass
            return h
        return g
    return inner
print(outer().__qualname__)
print(outer()().__qualname__)
print(outer()()().__qualname__)
