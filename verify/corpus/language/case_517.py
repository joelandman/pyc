# corpus case — ground truth is CPython at run time (CHARTER I5).
for w in (5, 6, 7, 8, 9):
    print(repr("ab".center(w, "*")), repr("abc".center(w, "*")))
print(repr("ab".center(2, "*")), repr("ab".center(1, "*")))
print(repr("x".center(4)))
def f(s, w): return s.center(w, "-")
print(f("ab", 7), f("abc", 7))
