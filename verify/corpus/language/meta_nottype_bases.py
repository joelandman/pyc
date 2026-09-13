class ANotMeta:
    def __new__(mcls, *args, **kwargs):
        return super().__new__(mcls)
class BNotMeta(ANotMeta):
    def __new__(mcls, *args, **kwargs):
        return super().__new__(mcls)
class A(metaclass=ANotMeta):
    pass
class B(metaclass=BNotMeta):
    pass
class C(A, B):
    pass
print(type(C).__name__)
class C2(B, A):
    pass
print(type(C2).__name__)
