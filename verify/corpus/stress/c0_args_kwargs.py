# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: *args, **kwargs, keyword-only and positional-only boundaries.
def f(a, b, /, c, d=10, *args, e, f_kw=20, **kwargs):
    return (a, b, c, d, args, e, f_kw, sorted(kwargs))


print(f(1, 2, 3))
print(f(1, 2, 3, 4, 5, 6, e=7))
print(f(1, 2, 3, e=9, x=1, y=2))
try:
    f(1, b=2, c=3)
    print("NO-ERR")
except TypeError:
    print("TypeError")
