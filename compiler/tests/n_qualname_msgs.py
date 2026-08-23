class C:
    def foo(self,a): pass
    class D:
        def bar(self,a): pass
def outer():
    def inner(a): pass
    try: inner()
    except TypeError as e: print(e)
outer()
f=lambda a:a
try: f()
except TypeError as e: print(e)
try: C().foo()
except TypeError as e: print(e)
try: C.D().bar()
except TypeError as e: print(e)
