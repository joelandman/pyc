# W8.4 / I-152 I-153 I-154: hasattr stored-None; tuple/reversed(None); list(instance).
class C:
    pass
o = C()
o.x = None
print(hasattr(o, "x"))
print(hasattr(o, "missing"))
try:
    print(tuple(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(reversed(None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(C()))
except TypeError as e:
    print(type(e).__name__)
print(tuple())
print(tuple("ab"))
print(list(reversed([1, 2])))
