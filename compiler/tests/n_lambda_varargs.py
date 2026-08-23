def app(f, xs): return f(*xs)
print(app(lambda *a: len(a), [1,2,3]))
print((lambda *a: a)(1,2,3))
print((lambda **k: sorted(k.items()))(b=2, a=1))
print((lambda x, *a, **k: (x, a, sorted(k)))(1, 2, 3, z=9))
print((lambda x, y=5: x+y)(1))
print((lambda x, y=5, *a: (x,y,a))(1,2,3,4))
n = 7
print((lambda *a: sum(a)+n)(1,2))
print(list(map(lambda *a: a, [1,2], [3,4])))
# positional-only lambda params are a recorded refusal, not a target:
# the trampoline binds keywords by name, so accepting them would let
# f(a=1) through where CPython raises TypeError.
