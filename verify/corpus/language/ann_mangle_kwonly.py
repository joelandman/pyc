class Spam:
    def f(self, *, __kw: 1):
        pass
class Ham(Spam):
    pass
print(Spam.f.__annotations__)
print(Ham.f.__annotations__)
