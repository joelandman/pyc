# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def f(self):
        s = super()
        try:
            print(len(s))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(1 in s)
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(s[0])
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(list(s))
        except TypeError as e:
            print(type(e).__name__)
C().f()
