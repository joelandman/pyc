from types import CellType

def run():
    result = 0
    def make():
        a = 2
        b = 3
        def f():
            nonlocal result
            nonlocal a
            nonlocal b
            result = a * b
        return f
    f = make()
    result = 0
    exec(f.__code__, f.__globals__, closure=f.__closure__)
    print(result)
    result = 0
    exec(f.__code__, f.__globals__, closure=(CellType(35), CellType(72), f.__closure__[2]))
    print(result)

run()
