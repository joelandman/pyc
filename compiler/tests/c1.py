def make_adder(n):
    def add(x):
        return x + n
    return add
a5 = make_adder(5)
print(a5(10))
print(make_adder(100)(1))
