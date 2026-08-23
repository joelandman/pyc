x=5
print(x if x>0 else -1)
print(-3 if False else "e")
try:
    def bad(x): return x.nope()
    print(bad(1))
except AttributeError as e:
    print(type(e).__name__)
for i in [1]:
    def g(): return i
    print(g())
