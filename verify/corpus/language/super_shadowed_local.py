def test():
    class super:
        msg = "quite super"
    class C:
        def method(self):
            return super().msg
    print(C().method())
test()
