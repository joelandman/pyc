class C:
    def m(self):
        return (x for x in range(1))
print(C().m().__qualname__)
print((x for x in range(1)).__qualname__)
def f():
    return (lambda: (x for x in range(1)))()
print(f().__qualname__)
