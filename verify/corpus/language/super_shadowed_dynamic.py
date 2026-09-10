class MySuper:
    msg = "super super"
class C:
    def method(self):
        return super().msg
super = MySuper
print(C().method())
