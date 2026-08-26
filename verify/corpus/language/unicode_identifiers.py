# unicode identifiers are NFKC-normalized
été = 5
print(repr(été))
ﬁeld = 7           # 'fi' ligature normalizes to 'field'
print(repr(field))
μ = 1
print(repr(µ))     # MICRO SIGN normalizes to GREEK SMALL MU
class C:
    é = 3
print(repr(C.é), repr(getattr(C, 'é')))
d = {'é': 1}
print(repr(d))
def f(été=2):
    return été
print(repr(f()), repr(f(été=9)))
