# W8.2 / I-145 I-149: getattr stored-None; *None / list(None).
class C:
    pass
o = C()
o.x = None
print(getattr(o, "x", 7))
print(getattr(o, "missing", 7))
def f(a=1):
    return a
try:
    print(f(*None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(f(*1))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(1))
except TypeError as e:
    print(type(e).__name__)
print(list())
print(list("ab"))
