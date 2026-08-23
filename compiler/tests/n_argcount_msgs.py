def f(a): pass
def g(a,b): pass
def h(a,b,c): pass
def k(a,b=1): pass
for fn,args in [(f,()),(g,()),(h,()),(k,(1,2,3))]:
    try: fn(*args)
    except TypeError as e: print(e)
try: g(1,2,3)
except TypeError as e: print(e)
