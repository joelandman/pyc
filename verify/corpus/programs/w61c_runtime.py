# W6.1c: close remaining Runtime str/super/SetItem wrong-answers (I-126–I-128).
class C127:
    def f(self):
        s = super()
        try:
            print(set(s))
        except TypeError as e:
            print("set", type(e).__name__)
        try:
            print(repr("".join(s)))
        except TypeError as e:
            print("join", type(e).__name__)
        print("eq", s == ())
        print("bool", bool(s))
C127().f()
print(repr("a\x00b".casefold()))
print(repr("a\x00b".ljust(5)))
print(repr("a\x00b".capitalize()))
try:
    1[None] = 2
    print("nonekey-ok")
except TypeError as e:
    print("nonekey", type(e).__name__)
