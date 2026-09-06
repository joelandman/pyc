X = 1
def f():
    a = 2
    b = 'é'
    print(sorted(locals().items()))
    print('X' in globals(), 'f' in globals())
    return len(locals())
print(f())
print(sorted(k for k in globals() if not k.startswith('__')))
class C:
    y = 3
    print(sorted(k for k in locals() if not k.startswith('__')))
print(sorted(k for k in vars(C) if not k.startswith('__')))
def g():
    n = 5
    return eval('n + 1', globals(), locals())
print(g())
