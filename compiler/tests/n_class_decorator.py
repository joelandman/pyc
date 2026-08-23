def tag(cls):
    cls.tagged = True
    return cls
@tag
class C:
    pass
print(C.tagged)
