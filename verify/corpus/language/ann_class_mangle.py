class C:
    __foo: int
    s: str = "attr"
print(sorted(C.__annotations__))
